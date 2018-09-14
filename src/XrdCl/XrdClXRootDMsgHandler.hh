//------------------------------------------------------------------------------
// Copyright (c) 2011-2014 by European Organization for Nuclear Research (CERN)
// Author: Lukasz Janyst <ljanyst@cern.ch>
//------------------------------------------------------------------------------
// This file is part of the XRootD software suite.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//
// In applying this licence, CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
//------------------------------------------------------------------------------

#ifndef __XRD_CL_XROOTD_MSG_HANDLER_HH__
#define __XRD_CL_XROOTD_MSG_HANDLER_HH__

#include "XrdCl/XrdClPostMasterInterfaces.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClDefaultEnv.hh"
#include "XrdCl/XrdClMessage.hh"
#include "XProtocol/XProtocol.hh"

#include <sys/uio.h>

#include <list>
#include <memory>

namespace XrdCl
{
  class PostMaster;
  class SIDManager;
  class URL;
  class LocalFileHandler;

  //----------------------------------------------------------------------------
  // Single entry in the redirect-trace-back
  //----------------------------------------------------------------------------
  struct RedirectEntry
  {
      RedirectEntry( const URL &from, const URL &to ) : from( from ), to( to )
      {

      }

      URL          from;
      URL          to;
      XRootDStatus status;

      std::string ToString( bool prevok = true )
      {
        const std::string tostr   = to.GetLocation();
        const std::string fromstr = from.GetLocation();

        if( prevok )
        {
          if( tostr == fromstr )
            return "Retrying: " + tostr;
          return "Redirected from: " + fromstr + " to: " + tostr;
        }
        return "Failed at: " + fromstr + ", retrying at: " + tostr;
      }
  };

  class XRootDMsgHandler;

  //----------------------------------------------------------------------------
  // Counted reference to XRootDMsgHandler, to be used with WaitTask
  //----------------------------------------------------------------------------
  class MsgHandlerRef
  {
    public:

      MsgHandlerRef( XRootDMsgHandler *handler) : ref( handler ), count( 1 )
      {

      }

      XRootDMsgHandler* operator->()
      {
        return ref;
      }

      operator bool() const
      {
        return ref;
      }

      operator XrdSysMutex&()
      {
        return mtx;
      }

      MsgHandlerRef& Self()
      {
        XrdSysMutexHelper lck( mtx );
        ++count;
        return *this;
      }

      void Invalidate()
      {
        XrdSysMutexHelper lck( mtx );
        ref = 0;
      }

      void Free()
      {
        XrdSysMutexHelper lck( mtx );
        --count;
        if( count == 0 )
        {
          lck.UnLock();
          delete this;
        }
      }

    private:

      XrdSysMutex       mtx;
      XRootDMsgHandler *ref;
      uint16_t          count;
  };

  //----------------------------------------------------------------------------
  //! Handle/Process/Forward XRootD messages
  //----------------------------------------------------------------------------
  class XRootDMsgHandler: public IncomingMsgHandler,
                          public OutgoingMsgHandler
  {
      friend class HandleRspJob;

    public:
      //------------------------------------------------------------------------
      //! Constructor
      //!
      //! @param msg         message that has been sent out
      //! @param respHandler response handler to be called then the final
      //!                    final response arrives
      //! @param url         the url the message has been sent to
      //! @param sidMgr      the sid manager used to allocate SID for the initial
      //!                    message
      //------------------------------------------------------------------------
      XRootDMsgHandler( Message          *msg,
                        ResponseHandler  *respHandler,
                        const URL        *url,
                        SIDManager       *sidMgr,
                        LocalFileHandler *lFileHandler):
        pRequest( msg ),
        pResponse( 0 ),
        pResponseHandler( respHandler ),
        pUrl( *url ),
        pSidMgr( sidMgr ),
        pLFileHandler( lFileHandler ),
        pExpiration( 0 ),
        pRedirectAsAnswer( false ),
        pHosts( 0 ),
        pHasLoadBalancer( false ),
        pHasSessionId( false ),
        pChunkList( 0 ),
        pRedirectCounter( 0 ),

        pAsyncOffset( 0 ),
        pAsyncReadSize( 0 ),
        pAsyncReadBuffer( 0 ),
        pAsyncMsgSize( 0 ),

        pReadRawStarted( false ),
        pReadRawCurrentOffset( 0 ),

        pReadVRawMsgOffset( 0 ),
        pReadVRawChunkHeaderDone( false ),
        pReadVRawChunkHeaderStarted( false ),
        pReadVRawSizeError( false ),
        pReadVRawChunkIndex( 0 ),
        pReadVRawMsgDiscard( false ),

        pOtherRawStarted( false ),

        pFollowMetalink( false ),

        pStateful( false ),

        pAggregatedWaitTime( 0 ),

        pMsgInFly( false ),

        pRef( new MsgHandlerRef( this ) )
      {
        pPostMaster = DefaultEnv::GetPostMaster();
        if( msg->GetSessionId() )
          pHasSessionId = true;
        memset( &pReadVRawChunkHeader, 0, sizeof( readahead_list ) );
      }

      //------------------------------------------------------------------------
      //! Destructor
      //------------------------------------------------------------------------
      ~XRootDMsgHandler()
      {
        pRef->Free();

        DumpRedirectTraceBack();

        if( !pHasSessionId )
          delete pRequest;
        delete pResponse;
        std::vector<Message *>::iterator it;
        for( it = pPartialResps.begin(); it != pPartialResps.end(); ++it )
          delete *it;
      }

      //------------------------------------------------------------------------
      //! Examine an incoming message, and decide on the action to be taken
      //!
      //! @param msg    the message, may be zero if receive failed
      //! @return       action type that needs to be take wrt the message and
      //!               the handler
      //------------------------------------------------------------------------
      virtual uint16_t Examine( Message *msg  );

      //------------------------------------------------------------------------
      //! Get handler sid
      //!
      //! return sid of the corresponding request, otherwise 0
      //------------------------------------------------------------------------
      virtual uint16_t GetSid() const;

      //------------------------------------------------------------------------
      //! Process the message if it was "taken" by the examine action
      //!
      //! @param msg the message to be processed
      //------------------------------------------------------------------------
      virtual void Process( Message *msg );

      //------------------------------------------------------------------------
      //! Read message body directly from a socket - called if Examine returns
      //! Raw flag - only socket related errors may be returned here
      //!
      //! @param msg       the corresponding message header
      //! @param socket    the socket to read from
      //! @param bytesRead number of bytes read by the method
      //! @return          stOK & suDone if the whole body has been processed
      //!                  stOK & suRetry if more data is needed
      //!                  stError on failure
      //------------------------------------------------------------------------
      virtual Status ReadMessageBody( Message  *msg,
                                      int       socket,
                                      uint32_t &bytesRead );

      //------------------------------------------------------------------------
      //! Handle an event other that a message arrival
      //!
      //! @param event     type of the event
      //! @param streamNum stream concerned
      //! @param status    status info
      //------------------------------------------------------------------------
      virtual uint8_t OnStreamEvent( StreamEvent event,
                                     uint16_t    streamNum,
                                     Status      status );

      //------------------------------------------------------------------------
      //! The requested action has been performed and the status is available
      //------------------------------------------------------------------------
      virtual void OnStatusReady( const Message *message,
                                  Status         status );

      //------------------------------------------------------------------------
      //! Are we a raw writer or not?
      //------------------------------------------------------------------------
      virtual bool IsRaw() const;

      //------------------------------------------------------------------------
      //! Write message body directly to a socket - called if IsRaw returns
      //! true - only socket related errors may be returned here
      //!
      //! @param socket    the socket to read from
      //! @param bytesRead number of bytes read by the method
      //! @return          stOK & suDone if the whole body has been processed
      //!                  stOK & suRetry if more data needs to be written
      //!                  stError on failure
      //------------------------------------------------------------------------
      Status WriteMessageBody( int       socket,
                               uint32_t &bytesRead );

      //------------------------------------------------------------------------
      //! Get message body - called if IsRaw returns true
      //!
      //! @param asyncOffset  :  the current async offset
      //! @return             :  the list of chunks
      //------------------------------------------------------------------------
      ChunkList* GetMessageBody( uint32_t *&asyncOffset )
      {
        asyncOffset = &pAsyncOffset;
        return pChunkList;
      }

      //------------------------------------------------------------------------
      //! Called after the wait time for kXR_wait has elapsed
      //!
      //! @param  now current timestamp
      //------------------------------------------------------------------------
      void WaitDone( time_t now );

      //------------------------------------------------------------------------
      //! Set a timestamp after which we give up
      //------------------------------------------------------------------------
      void SetExpiration( time_t expiration )
      {
        pExpiration = expiration;
      }

      //------------------------------------------------------------------------
      //! Treat the kXR_redirect response as a valid answer to the message
      //! and notify the handler with the URL as a response
      //------------------------------------------------------------------------
      void SetRedirectAsAnswer( bool redirectAsAnswer )
      {
        pRedirectAsAnswer = redirectAsAnswer;
      }

      //------------------------------------------------------------------------
      //! Get the request pointer
      //------------------------------------------------------------------------
      const Message *GetRequest() const
      {
        return pRequest;
      }

      //------------------------------------------------------------------------
      //! Set the load balancer
      //------------------------------------------------------------------------
      void SetLoadBalancer( const HostInfo &loadBalancer )
      {
        if( !loadBalancer.url.IsValid() )
          return;
        pLoadBalancer    = loadBalancer;
        pHasLoadBalancer = true;
      }

      //------------------------------------------------------------------------
      //! Set host list
      //------------------------------------------------------------------------
      void SetHostList( HostList *hostList )
      {
        delete pHosts;
        pHosts = hostList;
      }

      //------------------------------------------------------------------------
      //! Set the chunk list
      //------------------------------------------------------------------------
      void SetChunkList( ChunkList *chunkList )
      {
        pChunkList = chunkList;
        if( chunkList )
          pChunkStatus.resize( chunkList->size() );
        else
          pChunkStatus.clear();
      }

      //------------------------------------------------------------------------
      //! Set the redirect counter
      //------------------------------------------------------------------------
      void SetRedirectCounter( uint16_t redirectCounter )
      {
        pRedirectCounter = redirectCounter;
      }

      void SetFollowMetalink( bool followMetalink )
      {
        pFollowMetalink = followMetalink;
      }

      void SetStateful( bool stateful )
      {
        pStateful = stateful;
      }

    private:
      //------------------------------------------------------------------------
      //! Handle a kXR_read in raw mode
      //------------------------------------------------------------------------
      Status ReadRawRead( Message  *msg,
                          int       socket,
                          uint32_t &bytesRead );

      //------------------------------------------------------------------------
      //! Handle a kXR_readv in raw mode
      //------------------------------------------------------------------------
      Status ReadRawReadV( Message  *msg,
                           int       socket,
                           uint32_t &bytesRead );

      //------------------------------------------------------------------------
      //! Handle anything other than kXR_read and kXR_readv in raw mode
      //------------------------------------------------------------------------
      Status ReadRawOther( Message  *msg,
                           int       socket,
                           uint32_t &bytesRead );

      //------------------------------------------------------------------------
      //! Read a buffer asynchronously - depends on pAsyncBuffer, pAsyncSize
      //! and pAsyncOffset
      //------------------------------------------------------------------------
      Status ReadAsync( int socket, uint32_t &btesRead );

      //------------------------------------------------------------------------
      //! Recover error
      //------------------------------------------------------------------------
      void HandleError( Status status, Message *msg = 0 );

      //------------------------------------------------------------------------
      //! Retry the request at another server
      //------------------------------------------------------------------------
      Status RetryAtServer( const URL &url );

      //------------------------------------------------------------------------
      //! Unpack the message and call the response handler
      //------------------------------------------------------------------------
      void HandleResponse();

      //------------------------------------------------------------------------
      //! Extract the status information from the stuff that we got
      //------------------------------------------------------------------------
      XRootDStatus *ProcessStatus();

      //------------------------------------------------------------------------
      //! Parse the response and put it in an object that could be passed to
      //! the user
      //------------------------------------------------------------------------
      Status ParseResponse( AnyObject *&response );

      //------------------------------------------------------------------------
      //! Parse the response to kXR_fattr request and put it in an object that
      //! could be passed to the user
      //------------------------------------------------------------------------
      Status ParseXAttrResponse( char *data, size_t len, AnyObject *&response );

      //------------------------------------------------------------------------
      //! Perform the changes to the original request needed by the redirect
      //! procedure - allocate new streamid, append redirection data and such
      //------------------------------------------------------------------------
      Status RewriteRequestRedirect( const URL &newUrl );

      //------------------------------------------------------------------------
      //! Some requests need to be rewritten also after getting kXR_wait - sigh
      //------------------------------------------------------------------------
      Status RewriteRequestWait();

      //------------------------------------------------------------------------
      //! Post process vector read
      //------------------------------------------------------------------------
      Status PostProcessReadV( VectorReadInfo *vReadInfo );

      //------------------------------------------------------------------------
      //! Unpack a single readv response
      //------------------------------------------------------------------------
      Status UnPackReadVResponse( Message *msg );

      //------------------------------------------------------------------------
      //! Update the "tried=" part of the CGI of the current message
      //------------------------------------------------------------------------
      void UpdateTriedCGI(uint32_t errNo=0);

      //------------------------------------------------------------------------
      //! Switch on the refresh flag for some requests
      //------------------------------------------------------------------------
      void SwitchOnRefreshFlag();

      //------------------------------------------------------------------------
      //! If the current thread is a worker thread from our thread-pool
      //! handle the response, otherwise submit a new task to the thread-pool
      //------------------------------------------------------------------------
      void HandleRspOrQueue();

      //------------------------------------------------------------------------ 
      //! Handle a redirect to a local file
      //------------------------------------------------------------------------
      void HandleLocalRedirect( URL *url );

      //------------------------------------------------------------------------
      //! Check if it is OK to retry this request
      //!
      //! @param   reuqest : the request in question
      //! @return          : true if yes, false if no
      //------------------------------------------------------------------------
      bool IsRetryable( Message *request );

      //------------------------------------------------------------------------
      //! Check if for given request and Metalink redirector  it is OK to omit
      //! the kXR_wait and proceed stright to the next entry in the Metalink file
      //!
      //! @param   reuqest : the request in question
      //! @param   url     : metalink URL
      //! @return          : true if yes, false if no
      //------------------------------------------------------------------------
      bool OmitWait( Message *request, const URL &url );

      //------------------------------------------------------------------------
      //! Dump the redirect-trace-back into the log file
      //------------------------------------------------------------------------
      void DumpRedirectTraceBack();
      
      //! Read data from buffer
      //!
      //! @param buffer : the buffer with data
      //! @param size   : the size of the buffer
      //! @param result : output parameter (data read)
      //! @return       : status of the operation
      //------------------------------------------------------------------------
      template<typename T>
      Status ReadFromBuffer( char *&buffer, size_t &buflen, T& result );

      //------------------------------------------------------------------------
      //! Read a string from buffer
      //!
      //! @param buffer : the buffer with data
      //! @param size   : the size of the buffer
      //! @param result : output parameter (data read)
      //! @return       : status of the operation
      //------------------------------------------------------------------------
      Status ReadFromBuffer( char *&buffer, size_t &buflen, std::string &result );

      //------------------------------------------------------------------------
      //! Read a string from buffer
      //!
      //! @param buffer : the buffer with data
      //! @param buflen : size of the buffer
      //! @param size   : size of the data to read
      //! @param result : output parameter (data read)
      //! @return       : status of the operation
      //------------------------------------------------------------------------
      Status ReadFromBuffer( char *&buffer, size_t &buflen, size_t size,
                             std::string &result );

      //------------------------------------------------------------------------
      // Helper struct for async reading of chunks
      //------------------------------------------------------------------------
      struct ChunkStatus
      {
        ChunkStatus(): sizeError( false ), done( false ) {}
        bool sizeError;
        bool done;
      };

      typedef std::list<std::unique_ptr<RedirectEntry>> RedirectTraceBack;

      Message                        *pRequest;
      Message                        *pResponse;
      std::vector<Message *>          pPartialResps;
      ResponseHandler                *pResponseHandler;
      URL                             pUrl;
      PostMaster                     *pPostMaster;
      SIDManager                     *pSidMgr;
      LocalFileHandler               *pLFileHandler;
      Status                          pStatus;
      Status                          pLastError;
      time_t                          pExpiration;
      bool                            pRedirectAsAnswer;
      HostList                       *pHosts;
      bool                            pHasLoadBalancer;
      HostInfo                        pLoadBalancer;
      bool                            pHasSessionId;
      std::string                     pRedirectUrl;
      ChunkList                      *pChunkList;
      std::vector<ChunkStatus>        pChunkStatus;
      uint16_t                        pRedirectCounter;

      uint32_t                        pAsyncOffset;
      uint32_t                        pAsyncReadSize;
      char*                           pAsyncReadBuffer;
      uint32_t                        pAsyncMsgSize;

      bool                            pReadRawStarted;
      uint32_t                        pReadRawCurrentOffset;

      uint32_t                        pReadVRawMsgOffset;
      bool                            pReadVRawChunkHeaderDone;
      bool                            pReadVRawChunkHeaderStarted;
      bool                            pReadVRawSizeError;
      int32_t                         pReadVRawChunkIndex;
      readahead_list                  pReadVRawChunkHeader;
      bool                            pReadVRawMsgDiscard;

      bool                            pOtherRawStarted;

      bool                            pFollowMetalink;

      bool                            pStateful;
      int                             pAggregatedWaitTime;

      std::unique_ptr<RedirectEntry>  pRdirEntry;
      RedirectTraceBack               pRedirectTraceBack;

      bool                            pMsgInFly;

      //------------------------------------------------------------------------
      // (Counted) Reference to myself - passed to WaitTask
      //------------------------------------------------------------------------
      MsgHandlerRef                  *pRef;
  };
}

#endif // __XRD_CL_XROOTD_MSG_HANDLER_HH__
