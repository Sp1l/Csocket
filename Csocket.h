/**
*
*    Copyright (c) 1999-2003 Jim Hull <imaginos@imaginos.net>
*    All rights reserved
*
*    CVS Info:
*       $Author: imaginos $
*       $Date: 2003/09/05 01:14:29 $
*       $Revision: 1.27 $
*/

#ifndef _HAS_CSOCKET_
#define _HAS_CSOCKET_
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/timeb.h>

#ifdef HAVE_LIBSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif /* HAVE_LIBSSL */

#include "Cstring.h"
#include "Cthreads.h"
#include "Cmisc.h"

#define CS_BLOCKSIZE	64

// wrappers for FD_SET and such to work in templates
inline void TFD_ZERO( fd_set *set )
{
	FD_ZERO( set );
}

inline void TFD_SET( int iSock, fd_set *set )
{
	FD_SET( iSock, set );
}

inline bool TFD_ISSET( int iSock, fd_set *set )
{
	if ( FD_ISSET( iSock, set ) )
		return( true );
		
	return( false );
}

inline void TFD_CLR( int iSock, fd_set *set )
{
	FD_CLR( iSock, set );
}

inline bool GetHostByName( const Cstring & sHostName, struct in_addr *paddr )
{
	bool bRet = false;
	struct hostent *hent = NULL;
#ifdef __linux__
	char hbuff[2048];
	struct hostent hentbuff;
			
	int err;
	while( true )
	{
		memset( (char *)hbuff, '\0', 2048 );
		int iRet = gethostbyname_r( sHostName.c_str(), &hentbuff, hbuff, 2048, &hent, &err );
	
		if ( iRet == 0 )
		{
			bRet = true;
			break;
		}	
		if ( iRet != EAGAIN )
			break;
	}

#else
	static Cmutex m;

	m.lock();
	hent = gethostbyname( sHostName.c_str() );

	if ( hent )
		bRet = true;
	
#endif /* __linux__ */

	if ( bRet )
		*paddr = *(struct in_addr *)hent->h_addr; 

#ifndef __linux__
	m.unlock();
#endif /* __linux__ */

	return( bRet );
}
/**
* @class CCron
* @brief this is the main cron job class
*
* You should derive from this class, and override RunJob() with your code
* @author Jim Hull <imaginos@imaginos.net>
*/

class CCron
{
public:
		CCron() 
		{
			m_bOnlyOnce = true;
			m_bActive = true;
			m_iTime	= 0;
			m_iTimeSequence = 60;
		}
		virtual ~CCron() {}
		
		//! This is used by the Job Manager, and not you directly
		void run() 
		{
			if ( ( m_bActive ) && ( time( NULL ) >= m_iTime ) )
			{
			
				RunJob();

				if ( m_bOnlyOnce )
					m_bActive = false;
				else
					m_iTime = time( NULL ) + m_iTimeSequence;
			}
		}
		
		//! This is used by the Job Manager, and not you directly
		void Start( int TimeSequence, bool bOnlyOnce = true ) 
		{
			m_iTimeSequence = TimeSequence;
			m_iTime = time( NULL ) + m_iTimeSequence;
			m_bOnlyOnce = bOnlyOnce;
		}

		//! call this to turn off your cron, it will be removed
		void Stop()
		{
			m_bActive = false;
		}

		//! returns true if cron is active
		bool isValid() { return( m_bActive ); }
		
protected:

		//! this is the method you should override
		virtual void RunJob()
		{
			throw Cstring( "Override me fool" );
		}
		
		time_t	m_iTime;
		bool	m_bOnlyOnce, m_bActive;
		int		m_iTimeSequence;
};

/**
* @class Csock
* @brief Basic Socket Class
* The most basic level socket class
* You can use this class directly for quick things
* or use the socket manager
* @see TSocketManager
* @author Jim Hull <imaginos@imaginos.net>
*/
class Csock
{
public:
		//! default constructor, sets a timeout of 60 seconds
		Csock( int itimeout = 60 ) 
		{ 
			Init( "", 0, itimeout ); 
		}
		/**
		* Advanced constructor, for creating a simple connection
		*
		* sHostname the hostname your are connecting to
		* iport the port you are connectint to
		* itimeout how long to wait before ditching the connection, default is 60 seconds
		*/
		Csock( const Cstring & sHostname, int iport, int itimeout = 60 ) 
		{
			Init( sHostname, iport, itimeout );
		}
		
		// override this for accept sockets
		virtual Csock *GetSockObj( const Cstring & sHostname, int iPort ) 
		{ 
			return( NULL ); 
		}
		
		virtual ~Csock()
		{
			close( m_isock );
#ifdef HAVE_LIBSSL
			FREE_SSL();
			FREE_CTX();

#endif /* HAVE_LIBSSL */				
			// delete any left over crons
			for( unsigned int i = 0; i < m_vcCrons.size(); i++ )
				Zzap( m_vcCrons[i] );
		}

		enum ETConn
		{
			OUTBOUND	= 0,		//!< outbound connection
			LISTENER	= 1,		//!< a socket accepting connections
			INBOUND		= 2			//!< an inbound connection, passed from LISTENER
			
		};
		
		enum EFRead
		{
			READ_EOF	= 0,		//!< End Of File, done reading
			READ_ERR	= -1,		//!< Error on the socket, socket closed, done reading
			READ_EAGAIN	= -2		//!< Try to get data again
			
		};
		
		enum EFSelect
		{
			SEL_OK		= 0,		//!< Select passed ok
			SEL_TIMEOUT	= -1,		//!< Select timed out
			SEL_EAGAIN	= -2,		//!< Select wants you to try again
			SEL_ERR		= -3		//!< Select recieved an error
		
		};
		
		enum ESSLMethod
		{
			SSL23	= 0,
			SSL2	= 2,
			SSL3	= 3
		
		};

		Csock & operator<<( const Cstring & s ) 
		{
			Write( s );
			return( *this );
		}
		Csock & operator<<( ostream & ( *io )( ostream & ) )
		{
			Write( "\r\n" );
			return( *this );
		}			
		Csock & operator<<( int i ) 
		{
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}
		Csock & operator<<( unsigned int i ) 
		{
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}
		Csock & operator<<( long i ) 
		{ 
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}
		Csock & operator<<( unsigned long i ) 
		{
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}
		Csock & operator<<( unsigned long long i ) 
		{
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}
		Csock & operator<<( float i ) 
		{
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}
		Csock & operator<<( double i ) 
		{
			Write( Cstring::num2Cstring( i ) );
			return( *this );
		}		

		/**
		* Create the connection
		*
		* \param sBindHost the ip you want to bind to locally
		* \return true on success
		*/
		virtual bool Connect( const Cstring & sBindHost = "" )
		{
			// create the socket
			m_isock = SOCKET();
				
			m_address.sin_family = PF_INET;
			m_address.sin_port = htons( m_iport );

			if ( !GetHostByName( m_shostname, &(m_address.sin_addr) ) )
				return( false );

			// bind to a hostname if requested
			if ( !sBindHost.empty() )
			{
				struct sockaddr_in vh;

				vh.sin_family = PF_INET;
				vh.sin_port = htons( 0 );

				if ( !GetHostByName( sBindHost, &(vh.sin_addr) ) )
					return( false );

				// try to bind 3 times, otherwise exit failure
				bool bBound = false;
				for( int a = 0; a < 3; a++ )
				{
					if ( bind( m_isock, (struct sockaddr *) &vh, sizeof( vh ) ) == 0 )
					{
						bBound = true;
						break;
					}
					usleep( 5000 );	// quick pause, common lets BIND!)(!*!
				}
					
				if ( !bBound )
				{
					ERROR( "Failure to bind to " + sBindHost );
					return( false );
				}
			}
			
			// set it none blocking
			int fdflags = fcntl (m_isock, F_GETFL, 0);
			fcntl( m_isock, F_SETFL, fdflags|O_NONBLOCK );

			m_iConnType = OUTBOUND;
			
			// connect
			int ret = connect( m_isock, (struct sockaddr *)&m_address, sizeof( m_address ) );

			if ( ( ret == -1 ) && ( errno != EINPROGRESS ) )
				return( false );

			if ( m_bBLOCK )
			{	
				// unset the flags afterwords, rather than have connect block
				int fdflags = fcntl (m_isock, F_GETFL, 0); 
				
				if ( O_NONBLOCK & fdflags )
					fdflags -= O_NONBLOCK;
					
				fcntl( m_isock, F_SETFL, fdflags );
				
			}
			
			return( true );
		}
		
		/**
		* WriteSelect on this socket
		* Only good if JUST using this socket, otherwise use the TSocketManager
		*/
		virtual int WriteSelect()
		{
			struct timeval tv;
			fd_set wfds;

			TFD_ZERO( &wfds );
			TFD_SET( m_isock, &wfds );
			
			tv.tv_sec = m_itimeout;
			tv.tv_usec = 0;

			int ret = select( FD_SETSIZE, NULL, &wfds, NULL, &tv );

			if ( ret == 0 )
				return( SEL_TIMEOUT );
		
			if ( ret == -1 )
			{
				if ( errno == EINTR )
					return( SEL_EAGAIN );
				else
					return( SEL_ERR );
			}
			
			return( SEL_OK );
		}

		/**
		* ReadSelect on this socket
		* Only good if JUST using this socket, otherwise use the TSocketManager
		*/
		virtual int ReadSelect()
		{
			struct timeval tv;
			fd_set rfds;

			TFD_ZERO( &rfds );
			TFD_SET( m_isock, &rfds );

			tv.tv_sec = m_itimeout;
			tv.tv_usec = 0;
			
			int ret = select( FD_SETSIZE, &rfds, NULL, NULL, &tv );
			
			if ( ret == 0 )
				return( SEL_TIMEOUT );
		
			if ( ret == -1 )
			{
				if ( errno == EINTR )
					return( SEL_EAGAIN );
				else
					return( SEL_ERR );
			}
			
			return( SEL_OK );
		}
				
		/**
		* Listens for connections
		*
		* \param iPort the port to listen on
		* \param iMaxConns the maximum amount of connections to allow
		*/
		virtual bool Listen( int iPort, int iMaxConns = SOMAXCONN, const Cstring & sBindHost = "" )
		{
			m_isock = SOCKET( true );

			if ( m_isock == -1 )
				return( false );

			m_address.sin_family = PF_INET;
			if ( sBindHost.empty() )
				m_address.sin_addr.s_addr = htonl( INADDR_ANY );
			else
			{
				if ( !GetHostByName( sBindHost, &(m_address.sin_addr) ) )
					return( false );
			}
			m_address.sin_port = htons( iPort );
			bzero(&(m_address.sin_zero), 8);

			if ( bind(m_isock, (struct sockaddr *) &m_address, sizeof( m_address ) ) == -1 )
			{
				return( false );
			}
        
			if ( listen( m_isock, iMaxConns ) == -1 )
				return( false );

			if ( !m_bBLOCK )
			{
				// set it none blocking
				int fdflags = fcntl (m_isock, F_GETFL, 0);
				fcntl( m_isock, F_SETFL, fdflags|O_NONBLOCK );
			}
			
			m_iConnType = LISTENER;
			return( true );
		}
		
		//! Accept an inbound connection, this is used internally
		virtual int Accept( Cstring & sHost, int & iRPort )
		{
			struct sockaddr_in client;
			socklen_t clen = sizeof(struct sockaddr);
			
			int iSock = accept( m_isock , (struct sockaddr *) &client, &clen );
			
			if ( iSock != -1 )
			{
				if ( !m_bBLOCK )
				{
					// make it none blocking
					int fdflags = fcntl (iSock, F_GETFL, 0);
					fcntl( iSock, F_SETFL, fdflags|O_NONBLOCK );
				}
				
				getpeername( iSock, (struct sockaddr *) &client, &clen );
				
				sHost = inet_ntoa( client.sin_addr );
				iRPort = ntohs( client.sin_port );

				if ( !ConnectionFrom( sHost, iRPort ) )
				{
					close( iSock );
					iSock = -1;
				}

			}
			
			return( iSock );
		}
		
		//! Accept an inbound SSL connection, this is used internally and called after Accept
		virtual bool AcceptSSL()
		{
#ifdef HAVE_LIBSSL
			if ( !m_ssl )
				if ( !SSLServerSetup() )
					return( false );
				
			
			int err = SSL_accept( m_ssl );

			if ( err == 1 )
			{
				m_bFullsslAccept = true;
				return( true );				
			}

			m_bFullsslAccept = false;

			int sslErr = SSL_get_error( m_ssl, err );

			if ( ( sslErr == SSL_ERROR_WANT_READ ) || ( sslErr == SSL_ERROR_WANT_WRITE ) )
				return( true );

			ERR_print_errors_fp ( stderr );
			
#endif /* HAVE_LIBSSL */
			
			return( false );	
		}

		//! This sets up the SSL Client, this is used internally
		virtual bool SSLClientSetup()
		{
#ifdef HAVE_LIBSSL		
			m_bssl = true;
			FREE_SSL();
			FREE_CTX();

			SSLeay_add_ssl_algorithms();
	
			switch( m_iMethod )
			{
				case 0:
					m_ssl_method = SSLv23_client_method();
					if ( !m_ssl_method )
					{
						ERROR( "WARNING: MakeConnection .... SSLv23_client_method failed!" );
						return( false );
					}
					break;
		
				case 2:
					m_ssl_method = SSLv2_client_method();
					if ( !m_ssl_method )
					{
						ERROR( "WARNING: MakeConnection .... SSLv2_client_method failed!" );
						return( false );
					}
					break;
		
				case 3:
					m_ssl_method = SSLv3_client_method();
					if ( !m_ssl_method )
					{
						ERROR( "WARNING: MakeConnection .... SSLv3_client_method failed!" );
						return( false );
					}		
					break;
			
				default:
					ERROR( "WARNING: MakeConnection .... invalid http_sslversion version!" );
					return( false );
					break;
			}

			SSL_load_error_strings ();
			// wrap some warnings in here
			m_ssl_ctx = SSL_CTX_new ( m_ssl_method );
			if ( !m_ssl_ctx )
				return( false );
			
			m_ssl = SSL_new ( m_ssl_ctx );
			if ( !m_ssl )
				return( false );
			
			SSL_set_fd( m_ssl, m_isock );
			
			return( true );
#else
			return( false );
			
#endif /* HAVE_LIBSSL */		
		}

		//! This sets up the SSL Server, this is used internally
		virtual bool SSLServerSetup()
		{
#ifdef HAVE_LIBSSL
			m_bssl = true;
			FREE_SSL();			
			FREE_CTX();
					
			SSLeay_add_ssl_algorithms();
	
			switch( m_iMethod )
			{
				case 0:
					m_ssl_method = SSLv23_server_method();
					if ( !m_ssl_method )
					{
						ERROR( "WARNING: MakeConnection .... SSLv23_server_method failed!" );
						return( false );
					}
					break;
		
				case 2:
					m_ssl_method = SSLv2_server_method();
					if ( !m_ssl_method )
					{
						ERROR( "WARNING: MakeConnection .... SSLv2_server_method failed!" );
						return( false );
					}
					break;
		
				case 3:
					m_ssl_method = SSLv3_server_method();
					if ( !m_ssl_method )
					{
						ERROR( "WARNING: MakeConnection .... SSLv3_server_method failed!" );
						return( false );
					}		
					break;
			
				default:
					ERROR( "WARNING: MakeConnection .... invalid http_sslversion version!" ); 
					return( false );
					break;
			}

			SSL_load_error_strings ();
			// wrap some warnings in here
			m_ssl_ctx = SSL_CTX_new ( m_ssl_method );
			if ( !m_ssl_ctx )
				return( false );
			
			if ( ( m_sPemFile.empty() ) || ( access( m_sPemFile.c_str(), R_OK ) != 0 ) )
			{
				ERROR( "There is a problem with [" + m_sPemFile + "]" );
				return( false );
			}

			//
			// set up the CTX
			if ( SSL_CTX_use_certificate_file( m_ssl_ctx, m_sPemFile.c_str() , SSL_FILETYPE_PEM ) <= 0 )
			{
				ERROR( "Error with PEM file [" + m_sPemFile + "]" );
				/* print to char, report our naturally */
				ERR_print_errors_fp ( stderr );
				return( false );
			}
			
			if ( SSL_CTX_use_PrivateKey_file( m_ssl_ctx, m_sPemFile.c_str(), SSL_FILETYPE_PEM ) <= 0 )
			{
				ERROR( "Error with PEM file [" + m_sPemFile + "]" );
				// print out to char, report naturally
				ERR_print_errors_fp ( stderr );
				return( false );
			}
			
			if ( SSL_CTX_set_cipher_list( m_ssl_ctx, m_sCipherType.c_str() ) <= 0 )
			{
				ERROR( "Could not assign cipher [" + m_sCipherType + "]" );
				return( false );
			}

			//
			// setup the SSL
			m_ssl = SSL_new ( m_ssl_ctx );
			if ( !m_ssl )
				return( false );
			
			SSL_set_fd( m_ssl, m_isock );
			SSL_set_accept_state( m_ssl );
			
			return( true );
#else
			return( false );
#endif /* HAVE_LIBSSL */		
		}		

		/**
		* Create the SSL connection
		*
		* \param sBindhost the ip you want to bind to locally
		* \return true on success
		*/		
		virtual bool ConnectSSL( const Cstring & sBindhost = "" )
		{
#ifdef HAVE_LIBSSL		
			if ( m_isock == 0 )
				if ( !Connect( sBindhost ) )
					return( false );

			if ( !m_ssl )
				if ( !SSLClientSetup() )
					return( false );
				
			bool bPass = true;
			
			if ( m_bBLOCK )
			{
				int fdflags = fcntl (m_isock, F_GETFL, 0);
				fcntl( m_isock, F_SETFL, fdflags|O_NONBLOCK );
			}	
			
			int iErr = SSL_connect( m_ssl );
			if ( iErr != 1 )
			{
				int sslErr = SSL_get_error( m_ssl, iErr );

				if ( ( sslErr != SSL_ERROR_WANT_READ ) && ( sslErr != SSL_ERROR_WANT_WRITE ) )
					bPass = false;
			} else
				bPass = true;

			if ( m_bBLOCK )
			{	
				// unset the flags afterwords, rather then have connect block
				int fdflags = fcntl (m_isock, F_GETFL, 0); 
				
				if ( O_NONBLOCK & fdflags )
					fdflags -= O_NONBLOCK;
					
				fcntl( m_isock, F_SETFL, fdflags );
				
			}				
			
			return( bPass );
#else
			return( false );
#endif /* HAVE_LIBSSL */
		}


		/**
		* Write data to the socket
		* if not all of the data is sent, it will be stored on
		* an internal buffer, and tried again with next call to Write
		* if the socket is blocking, it will send everything
		*
		* \param data the data to send
		* \param len the length of data
		*/
		virtual bool Write( const char *data, int len )
		{
			m_sSend.append( data, len );

			if ( m_sSend.empty() )
			{
#ifdef _DEBUG_
				// maybe eventually make a __DEBUG_ONLY()
				ERROR( "ER, NOT Sending DATA!? [" + GetSockName() + "]" );
#endif /* _DEBUG_ */
				return( true );
			}
			
			if ( m_bBLOCK )
			{
				if ( WriteSelect() != SEL_OK )
					return( false );

			}
			// rate shaping
			u_int iBytesToSend = 0;
			
			if ( ( m_iMaxBytes > 0 ) && ( m_iMaxMilliSeconds > 0 ) )
			{
				unsigned long long iNOW = GetMillTime();
				// figure out the shaping here				
				// if NOW - m_iLastSendTime > m_iMaxMilliSeconds then send a full length of ( iBytesToSend )
				if ( ( iNOW - m_iLastSendTime ) > m_iMaxMilliSeconds )
				{
					m_iLastSendTime = iNOW;
					iBytesToSend = m_iMaxBytes;
					m_iLastSend = 0;
				
				} else // otherwise send m_iMaxBytes - m_iLastSend
					iBytesToSend = m_iMaxBytes - m_iLastSend;

				// take which ever is lesser
				if ( m_sSend.length() < iBytesToSend )
					iBytesToSend = 	m_sSend.length();
								
				// add up the bytes sent
				m_iLastSend += iBytesToSend;
				
				// so, are we ready to send anything ?
				if ( iBytesToSend == 0 )
					return( true );

			} else
				iBytesToSend = m_sSend.length();
				
			m_bNeverWritten = false;
#ifdef HAVE_LIBSSL	
			if ( m_bssl )
			{
				int iErr = SSL_write( m_ssl, m_sSend.c_str(), iBytesToSend );
				switch( SSL_get_error( m_ssl, iErr ) )
				{
					case SSL_ERROR_NONE:
					m_bsslEstablished = true;					
					// all ok
					break;
				
					case SSL_ERROR_ZERO_RETURN:
					// weird closer alert
					return( false );
					break;
				
					case SSL_ERROR_WANT_READ:
					// retry
					break;
				
					case SSL_ERROR_WANT_WRITE:
					// retry
					break;
						
					case SSL_ERROR_SSL:
					return( false );
					break;
				}

				if ( iErr > 0 )				
					m_sSend.erase( 0, iErr );
				
				return( true );
			}
#endif /* HAVE_LIBSSL */
			int bytes = write( m_isock, m_sSend.c_str(), iBytesToSend );
		
			if ( ( bytes <= 0 ) && ( errno != EAGAIN ) )
				return( false );
			
			// delete the bytes we sent
			if ( bytes > 0 )
				m_sSend.erase( 0, bytes );

			return( true );
		}
		
		/**
		* convience function
		* @see Write( const char *, int )
		*/
		virtual bool Write( const Cstring & sData )
		{
			return( Write( sData.c_str(), sData.length() ) );
		}

		/**
		* Read from the socket
		* Just pass in a pointer, big enough to hold len bytes
		*
		* \param data the buffer to read into
		* \param len the size of the buffer
		*
		* \return
		* Returns READ_EOF for EOF
		* Returns READ_ERR for ERROR
		* Returns READ_EAGAIN for Try Again ( EAGAIN )
		* Otherwise returns the bytes read into data
		*/
		virtual int Read( char *data, int len )
		{
	
			if ( m_bBLOCK )
			{
				if ( ReadSelect() != SEL_OK )
					return( READ_ERR );
			}
			
			int bytes = 0;
			memset( (char *)data, '\0', len );
			
#ifdef HAVE_LIBSSL
			if ( m_bssl )
				bytes = SSL_read( m_ssl, data, len );
			else
#endif /* HAVE_LIBSSL */
				bytes = read( m_isock, data, len );

		    if ( bytes == -1 )
			{
				if ( ( errno == EINTR ) || ( errno == EAGAIN ) )
					return( READ_EAGAIN );
#ifdef HAVE_LIBSSL
				if ( m_bssl )
				{
					int iErr = SSL_get_error( m_ssl, bytes );
					if ( ( iErr != SSL_ERROR_WANT_READ ) && ( iErr != SSL_ERROR_WANT_WRITE ) )
						return( READ_ERR );
					else
						return( READ_EAGAIN );
				}
#else
				return( READ_ERR );
#endif /* HAVE_LIBSSL */								
			}
			
			return( bytes );
		}

		//! Tells you if the socket is ready for write.
		virtual bool HasWrite() { return( m_bhaswrite ); }
		virtual void SetWrite( bool b ) { m_bhaswrite = b; }	

		/**
		* has never been written too, you can easy use the two to find a first case scenario
		* if ( ( HasWrite() ) && ( !NeedsWrite() ) )
		*		first time write
		*/		
		virtual bool NeedsWrite()
		{
			return( m_bNeverWritten );
		}

		//! returns a reference to the sock
		int & GetSock() { return( m_isock ); }
		void SetSock( int iSock ) { m_isock = iSock; }
		
		//! resets the time counter
		void ResetTimer() { m_iTcount = 0; }
		
		/**
		* this timeout isn't just connection timeout, but also timeout on
		* NOT recieving data, to disable this set it to 0
		* then the normal TCP timeout will apply (basically TCP will kill a dead connection)
		* Set the timeout, set to 0 to never timeout
		*/
		void SetTimeout( int iTimeout ) { m_itimeout = iTimeout; }
		
		//! returns true if the socket has timed out
		virtual bool CheckTimeout()
		{
			if ( m_itimeout > 0 )
			{
				if ( m_iTcount >= m_itimeout )
				{
					Timeout();
					return( true );
				}
				
				m_iTcount++;
			}	
			return( false );
		}
		
		/**
		* pushes data up on the buffer, if a line is ready
		* it calls the ReadLine event
		*/
		virtual void PushBuff( const char *data, int len )
		{
			if ( data )
				m_sbuffer.append( data, len );
			
			while( true )
			{
				u_int iFind = m_sbuffer.find( "\n" );
			
				if ( iFind != Cstring::npos )
				{
					Cstring sBuff = m_sbuffer.substr( 0, iFind + 1 );	// read up to(including) the newline
					m_sbuffer.erase( 0, iFind + 1 );					// erase past the newline
					ReadLine( sBuff );

				} else
					break;
			}
		}
		
		//! Returns the connection type from enum eConnType
		int GetType() { return( m_iConnType ); }
		void SetType( int iType ) { m_iConnType = iType; }
		
		//! Returns a reference to the socket name
		const Cstring & GetSockName() { return( m_sSockName ); }
		void SetSockName( const Cstring & sName ) { m_sSockName = sName; }
		
		//! Returns a reference to the host name
		const Cstring & GetHostName() { return( m_shostname ); }
		void SetHostName( const Cstring & sHostname ) { m_shostname = sHostname; }

		//! Returns the port
		int GetPort() { return( m_iport ); }
		void SetPort( int iPort ) { m_iport = iPort; }
		
		//! just mark us as closed, the parent can pick it up
		void Close() { m_bClosed = true; }
		//! returns true if the socket is closed
		bool isClosed() { return( m_bClosed ); }
		
		//! Set rather to NON Blocking IO on this socket, default is true
		void BlockIO( bool bBLOCK ) { m_bBLOCK = bBLOCK; }

		//! if this connection type is ssl or not
		bool GetSSL() { return( m_bssl ); }
		void SetSSL( bool b ) { m_bssl = b; }
		
		//! Set the cipher type ( openssl cipher [to see ciphers available] )
		void SetCipher( const Cstring & sCipher ) { m_sCipherType = sCipher; }
		const Cstring & GetCipher() { return( m_sCipherType ); }
		
		//! Set the pem file location
		void SetPemLocation( const Cstring & sPemFile ) { m_sPemFile = sPemFile; }
		const Cstring & GetPemLocation() { return( m_sPemFile ); }
		
		//! Set the SSL method type
		void SetSSLMethod( int iMethod ) { m_iMethod = iMethod; }
		int GetSSLMethod() { return( m_iMethod ); }
		
		//! Get the send buffer
		const Cstring & GetSendBuff() { return( m_sSend ); }

		//! is SSL_accept finished ?
		bool FullSSLAccept() { return ( m_bFullsslAccept ); }
		//! is the ssl properly finished (from write no error)
		bool SslIsEstablished() { return ( m_bsslEstablished ); }

		//! returns the underlying buffer
		Cstring & GetBuffer() { return( m_sSend ); }
		
		//! Get the peer's X509 cert
#ifdef HAVE_LIBSSL
		X509 *getX509()
		{
			if ( m_ssl )
				return( SSL_get_peer_certificate( m_ssl ) );

			return( NULL );
		}
#endif /* HAVE_LIBSSL */

		//! Set The INBOUND Parent sockname
		virtual void SetParentSockName( const Cstring & sParentName ) { m_sParentName = sParentName; }
		const Cstring & GetParentSockName() { return( m_sParentName ); }
		
		/*
		* sets the rate at which we can send data
		* \param iBytes the amount of bytes we can write
		* \param iMilliseconds the amount of time we have to rate to iBytes
		*/
		virtual void SetRate( u_int iBytes, unsigned long long iMilliseconds )
		{
			m_iMaxBytes = iBytes;
			m_iMaxMilliSeconds = iMilliseconds;
		}
		
		u_int GetRateBytes() { return( m_iMaxBytes ); }
		unsigned long long GetRateTime() { return( m_iMaxMilliSeconds ); }
		
		
		//! This has a garbage collecter, and is used internall to call the jobs
		virtual void Cron()
		{
			for( unsigned int a = 0; a < m_vcCrons.size(); a++ )
			{		
				CCron *pcCron = m_vcCrons[a];

				if ( !pcCron->isValid() )
				{
					Zzap( pcCron );
					m_vcCrons.erase( m_vcCrons.begin() + a-- );
				} else
					pcCron->run();
			}
		}
		
		//! insert a newly created cron
		virtual void AddCron( CCron * pcCron )
		{
			m_vcCrons.push_back( pcCron );
		}
				
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* Connected event
		*/
		virtual void Connected() {}
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* Disconnected event
		*/
		virtual void Disconnected() {}
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* Sock Timed out event
		*/
		virtual void Timeout() {}
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* Ready to read data event
		*/
		virtual void ReadData( const char *data, int len ) {}
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* Ready to Read a full line event
		*/
		virtual void ReadLine( const Cstring & sLine ) {}
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* A sock error occured event
		*/
		virtual void SockError() {}
		/**
		* Override these functions for an easy interface when using the Socket Manager
		* Don't bother using these callbacks if you are using this class directly (without Socket Manager)
		* as the Socket Manager calls most of these callbacks
		*
		* 
		* Incoming Connection Event
		* return false and the connection will fail
		* default returns true
		*/
		virtual bool ConnectionFrom( const Cstring & sHost, int iPort ) { return( true ); }

		//! return the data imediatly ready for read
		virtual int GetPending()
		{
#ifdef HAVE_LIBSSL
			if( m_ssl )
				return( SSL_pending( m_ssl ) );
			else
				return( 0 );
#else
			return( 0 );
#endif /* HAVE_LIBSSL */
		}

		//////////////////////////////////////////////////	
			
private:
		int			m_isock, m_itimeout, m_iport, m_iConnType, m_iTcount, m_iMethod;
		bool		m_bssl, m_bhaswrite, m_bNeverWritten, m_bClosed, m_bBLOCK, m_bFullsslAccept, m_bsslEstablished;
		Cstring		m_shostname, m_sbuffer, m_sSockName, m_sPemFile, m_sCipherType, m_sParentName;
		Cstring		m_sSend;

		unsigned long long	m_iMaxMilliSeconds, m_iLastSendTime;
		unsigned int		m_iMaxBytes, m_iLastSend;
		
		struct sockaddr_in 	m_address;
		
#ifdef HAVE_LIBSSL
		SSL 				*m_ssl;
		SSL_CTX				*m_ssl_ctx;
		SSL_METHOD			*m_ssl_method;

		virtual void FREE_SSL()
		{
			if ( m_ssl )
			{
				SSL_shutdown( m_ssl );
				SSL_free( m_ssl );
			}
			m_ssl = NULL;
		}

		virtual void FREE_CTX()
		{
			if ( m_ssl_ctx )
				SSL_CTX_free( m_ssl_ctx );

			m_ssl_ctx = NULL;
		}

#endif /* HAVE_LIBSSL */

		vector<CCron *>		m_vcCrons;

		//! Create the socket
		virtual int SOCKET( bool bListen = false )
		{
			int iRet = 0;
			while( iRet == 0 )
				iRet = socket( PF_INET, SOCK_STREAM, IPPROTO_TCP );
			
			if ( ( iRet > -1 ) && ( bListen ) )
			{
				const int on = 1;

                        	if ( setsockopt( iRet, SOL_SOCKET, SO_REUSEADDR, &on, sizeof( on ) ) != 0 )
				{
					ERROR( "SOCKET: Could not set SO_REUSEADDR" );
				}
			}

			return( iRet );
		}

		virtual void Init( const Cstring & sHostname, int iport, int itimeout = 60 )
		{
#ifdef HAVE_LIBSSL	
			m_ssl = NULL;
			m_ssl_ctx = NULL;
#endif /* HAVE_LIBSSL */		
			m_isock = 0;
			m_itimeout = itimeout;
			m_bssl = false;
			m_bhaswrite = false;
			m_iport = iport;
			m_shostname = sHostname;
			m_bNeverWritten = true;
			m_iTcount = 0;
			m_sbuffer.clear();
			m_bClosed = false;
			m_bBLOCK = true;
			m_iMethod = SSL23;
			m_sCipherType = "ALL";
			m_sPemFile = "server.pem";
			m_iMaxBytes = 0;
			m_iMaxMilliSeconds = 0;
			m_iLastSendTime = 0;
			m_iLastSend = 0;
			m_bFullsslAccept = false;
			m_bsslEstablished = false;
		}
};

/**
* @class TSocketManager
* @brief Best class to use to interact with the sockets
*
* handles SSL and NON Blocking IO
* Its a template class since Csock derives need to be new'd correctly
* Makes it easier to use overall
* Rather then use it directly, you'll probably get more use deriving from it
*
* class CBlahSock : public TSocketManager<SomeSock>
*
* @author Jim Hull <imaginos@imaginos.net>
*/

template<class T>
class TSocketManager : public vector<T *>
{
public:
		TSocketManager() 
		{ 
			m_errno = SUCCESS; 
			m_iCallTimeouts = GetMillTime();
			m_iSelectWait = 100000; // Default of 100 milliseconds
		}

		virtual ~TSocketManager() 
		{
			for( unsigned int i = 0; i < size(); i++ )
			{
				if ( (*this)[i] )
					Zzap( (*this)[i] );
			}
		}

		enum EMessages
		{
			SUCCESS			= 0,	//! Select returned more then 1 fd ready for action
			SELECT_ERROR	= -1,	//! An Error Happened, Probably dead socket. That socket is returned if available
			SELECT_TIMEOUT	= -2,	//! Select Timeout
			SELECT_TRYAGAIN	= -3	//! Select calls for you to try again
			
		};

		typedef struct
		{
			T			*pcSock;
			EMessages	eErrno;
		
		} stSock;		

		/**
		* Create a connection
		*
		* \param sHostname the destination
		* \param iPort the destination port
		* \param sSockName the Socket Name ( should be unique )
		* \param iTimeout the amount of time to try to connect
		* \param isSSL does the connection require a SSL layer
		* \param sBindHost the host to bind too
		* \return true on success
		*/
		virtual bool Connect( const Cstring & sHostname, int iPort , const Cstring & sSockName, int iTimeout = 60, bool isSSL = false, const Cstring & sBindHost = "", T *pcSock = NULL )
		{
			// create the new object
			if ( !pcSock )
				pcSock = new T( sHostname, iPort, iTimeout );
			else
			{
				pcSock->SetHostName( sHostname );
				pcSock->SetPort( iPort );
				pcSock->SetTimeout( iTimeout );
			}
			
			// make it NON-Blocking IO
			pcSock->BlockIO( false );
			
			if ( !pcSock->Connect( sBindHost ) )
			{
				Zzap( pcSock );
				return( false );
			}
	
#ifdef HAVE_LIBSSL
			if ( isSSL )
			{
				if ( !pcSock->ConnectSSL() )
				{
					Zzap( pcSock );
					return( false );
				}
			}
#endif /* HAVE_LIBSSL */
			
			AddSock( pcSock, sSockName );
			return( true );
		}

		/**
		* Create a listening socket
		* 
		* \param iPort the port to listen on
		* \param sSockName the name of the socket
		* \param isSSL if the sockets created require an ssl layer
		* \param iMaxConns the maximum amount of connections to accept
		* \return true on success
		*/
		virtual bool ListenHost( int iPort, const Cstring & sSockName, const Cstring & sBindHost, int isSSL = false, int iMaxConns = SOMAXCONN, T *pcSock = NULL )
		{
			if ( !pcSock )
				pcSock = new T();

			pcSock->BlockIO( false );

			pcSock->SetSSL( isSSL );

			if ( pcSock->Listen( iPort, iMaxConns, sBindHost ) )
			{
				AddSock( pcSock, sSockName );
				return( true );
			}
			Zzap( pcSock );
			return( false );
		}
	
		virtual bool ListenAll( int iPort, const Cstring & sSockName, int isSSL = false, int iMaxConns = SOMAXCONN, T *pcSock = NULL )
		{
			return( ListenHost( iPort, sSockName, "", isSSL, iMaxConns, pcSock ) );
		}

		/*
		* Best place to call this class for running, all the call backs are called
		* You should through this in your main while loop (long as its not blocking)
		* all the events are called as needed
		*/ 
		virtual void Loop ()
		{
			vector<stSock> vstSock = Select();
			map<T *, bool> mstSock;
			
			switch( m_errno )
			{
				case SUCCESS:
				{
					for( unsigned int a = 0; a < vstSock.size(); a++ )
					{
						T * pcSock = vstSock[a].pcSock;
						EMessages iErrno = vstSock[a].eErrno;
						
						// mark that this sock was ready
						mstSock[pcSock] = true;			
						if ( iErrno == SUCCESS )
						{					
							pcSock->ResetTimer();	// reset the timeout timer
							
							// read in data
							// if this is a 
							char *buff;
							int iLen = 0;
		
							if ( pcSock->GetSSL() )
								iLen = pcSock->GetPending();
		
							if ( iLen > 0 )
							{
								buff = (char *)malloc( iLen );
							} else
							{
								iLen = CS_BLOCKSIZE;
								buff = (char *)malloc( CS_BLOCKSIZE );
						
							}
		
							int bytes = pcSock->Read( buff, iLen );
		
							switch( bytes )
							{
								case 0:
								{
									// EOF
									DelSock( pcSock );
									break;
								}
								
								case -1:
								{
									pcSock->SockError();
									DelSock( pcSock );
									break;
								}
								
								case -2:
									break;
									
								default:
								{
									pcSock->PushBuff( buff, bytes );
									pcSock->ReadData( buff, bytes );
									break;
								}						
							}
							// free up the buff
							free( buff );
						
						} else if ( iErrno == SELECT_ERROR )
						{
							// a socket came back with an error
							// usually means it was closed
							DestroySock( pcSock );
						}
					}
					break;
				}
				
				case SELECT_TIMEOUT:
				case SELECT_ERROR:
				default	:
					break;
			}
			
			
			if ( ( GetMillTime() - m_iCallTimeouts ) > 1000 )
			{
				m_iCallTimeouts = GetMillTime();
				// call timeout on all the sockets that recieved no data
				for( unsigned int i = 0; i < size(); i++ )
				{
					if ( (*this)[i]->GetType() != T::LISTENER )
					{
						// are we in the map of found socks ?
						if ( mstSock.find( (*this)[i] ) == mstSock.end() )
						{
							if ( (*this)[i]->CheckTimeout() )
								DestroySock( (*this)[i] );
						}
					}
				}
			}
			// run any Manager Crons we may have
			Cron();
		}

		/*
		* Make this method virtual, so you can override it when a socket is added
		* Assuming you might want to do some extra stuff
		*/
		virtual void AddSock( T *pcSock, const Cstring & sSockName )
		{
			pcSock->SetSockName( sSockName );
			push_back( pcSock );
		}
		
		//! returns a pointer to the sock found by name or NULL on no match
		virtual T * FindSockByName( const Cstring & sName )
		{
			for( unsigned int i = 0; i < size(); i++ )
				if ( (*this)[i]->GetSockName() == sName )
					return( (*this)[i] );
			
			return( NULL );
		}
		
		//! returns a vector of pointers to socks with sHostname as being connected
		virtual vector<T *> FindSocksByRemoteHost( const Cstring & sHostname )
		{
			vector<T *> vpSocks;
			
			for( unsigned int i = 0; i < size(); i++ )
				if ( (*this)[i]->GetHostName() == sHostname )
					vpSocks.push_back( (*this)[i] );
			
			return( vpSocks );
		}
		
		//! return the last known error as set by this class
		int GetErrno() { return( m_errno ); }

		//! add a cronjob at the manager level
		virtual void AddCron( CCron *pcCron )
		{
			m_vcCrons.push_back( pcCron );
		}

		//! Get the Select Timeout in MILLISECONDS
		u_int GetSelectTimeout() { return( m_iSelectWait ); }
		//! Set the Select Timeout in MILLISECONDS
		void  SetSelectTimeout( u_int iTimeout ) { m_iSelectWait = iTimeout; }

private:
		/**
		* returns a pointer to the ready Csock class thats available
		* returns empty vector if none are ready, check GetErrno() for the error, if not SUCCESS Select() failed
		* each struct contains the socks error
		* @see GetErrno()
		*/
		virtual vector<stSock> Select()
		{		
			struct timeval tv;
			fd_set rfds, wfds;
			vector<stSock> vRet;
			
			tv.tv_sec = 0;
			tv.tv_usec = m_iSelectWait;
		
			TFD_ZERO( &rfds );						
			TFD_ZERO( &wfds );

			// before we go any further, Process work needing to be done on the job

			for( unsigned int i = 0; i < size(); i++ )
			{

				if ( (*this)[i]->isClosed() )
				{
					/*
					if ( (*this)[i]->GetType() == T::LISTENER )
						WARN( "Closing Listener" );
					*/

					DestroySock( (*this)[i] );

				} else
				{
					// call the Cron handler here
					(*this)[i]->Cron();
				}
			}

			for( unsigned int i = 0; i < m_pcDestroySocks.size(); i++ )
				DelSock( m_pcDestroySocks[i] );

			m_pcDestroySocks.clear();
			
			// on with the show
			
			bool bHasWriteable = false;

			for( unsigned int i = 0; i < size(); i++ )
			{

				Csock *pcSock = (*this)[i];
				
				if ( pcSock->GetType() != T::LISTENER )
				{
					int & iSock = pcSock->GetSock();

					if ( ( pcSock->GetSSL() ) && ( pcSock->GetType() == T::INBOUND ) && ( !pcSock->FullSSLAccept() ) )
					{
						// try accept on this socket again
						if ( !pcSock->AcceptSSL() )
							pcSock->Close();

					} else if ( ( pcSock->HasWrite() ) && ( pcSock->GetSendBuff().empty() ) )
					{
						TFD_SET( iSock, &rfds );
					
					} else if ( ( pcSock->GetSSL() ) && ( !pcSock->SslIsEstablished() ) && ( !pcSock->GetSendBuff().empty() ) )
					{
						// do this here, cause otherwise ssl will cause a small
						// cpu spike waiting for the handshake to finish
						TFD_SET( iSock, &rfds );
						// resend this data
						if ( !pcSock->Write( "" ) )
							pcSock->Close();
	
					} else 
					{
						TFD_SET( iSock, &rfds );
						TFD_SET( iSock, &wfds );
						bHasWriteable = true;
					}
				} else
				{
					Cstring sHost;
					int port;
					int inSock = pcSock->Accept( sHost, port );
					
					if ( inSock != -1 )
					{
						// if we have a new sock, then add it
						T *NewpcSock = (T *)pcSock->GetSockObj( sHost, port );

						if ( !NewpcSock )
							NewpcSock = new T( sHost, port );

						NewpcSock->BlockIO( false );
						
						NewpcSock->SetType( T::INBOUND );
						NewpcSock->SetSock( inSock );
						
						bool bAddSock = true;
#ifdef HAVE_LIBSSL						
						//
						// is this ssl ?
						if ( pcSock->GetSSL() )
						{
							NewpcSock->SetCipher( pcSock->GetCipher() );
							NewpcSock->SetPemLocation( pcSock->GetPemLocation() );
							bAddSock = NewpcSock->AcceptSSL();
						}
								
#endif /* HAVE_LIBSSL */
						if ( bAddSock )
						{
							// set the name of the listener
							NewpcSock->SetParentSockName( pcSock->GetSockName() );
							AddSock( NewpcSock,  sHost + ":" + Cstring::num2Cstring( port ) );
						
						} else
							Zzap( NewpcSock );
					}
				}
			}
		
			// first check to see if any ssl sockets are ready for immediate read
			// a mini select() type deal for ssl
			for( unsigned int i = 0; i < size(); i++ )
			{
				T *pcSock = (*this)[i];
		
				if ( ( pcSock->GetSSL() ) && ( pcSock->GetType() != Csock::LISTENER ) )
				{
					if ( pcSock->GetPending() > 0 )
						AddstSock( &vRet, SUCCESS, pcSock );
				}
			}

			// old fashion select, go fer it
			int iSel;

			if ( !vRet.empty() )
				tv.tv_usec = 1000;	// this won't be a timeout, 1 ms pause to see if anything else is ready
				
			if ( bHasWriteable )
				iSel = select(FD_SETSIZE, &rfds, &wfds, NULL, &tv);
			else
				iSel = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);

			if ( iSel == 0 )
			{
				if ( vRet.empty() )
					m_errno = SELECT_TIMEOUT;
				else
					m_errno = SUCCESS;
				
				return( vRet );
			}
			
			if ( ( iSel == -1 ) && ( errno == EINTR ) )
			{
				if ( vRet.empty() )
					m_errno = SELECT_TRYAGAIN;
				else
					m_errno = SUCCESS;
				
				return( vRet );				
			
			} else if ( iSel == -1 )
			{
				WARN( "Select Error!" );
				
				if ( vRet.empty() )
					m_errno = SELECT_ERROR;
				else
					m_errno = SUCCESS;
				
				return( vRet );
			
			} else
			{
				m_errno = SUCCESS;
			}							
			
			// find out wich one is ready
			for( unsigned int i = 0; i < size(); i++ )
			{
				T *pcSock = (*this)[i];
				int & iSock = pcSock->GetSock();
				EMessages iErrno = SUCCESS;
				
				if ( TFD_ISSET( iSock, &wfds ) )
				{
					if ( iSel > 0 )
					{
						iErrno = SUCCESS;
						
						if ( !pcSock->HasWrite() )
						{
							pcSock->SetWrite( true );
							// Call the Connected Event
							pcSock->Connected();
						}
						// write whats in the socks send buffer
						if ( !pcSock->GetSendBuff().empty() )
						{
							if ( !pcSock->Write( "" ) )
							{
								// write failed, sock died :(
								iErrno = SELECT_ERROR;
							}
						}
					} else
						iErrno = SELECT_ERROR;

					AddstSock( &vRet, iErrno, pcSock );

				} else if ( TFD_ISSET( iSock, &rfds ) )
				{
					if ( iSel > 0 )
						iErrno = SUCCESS;
					else
						iErrno = SELECT_ERROR;

					AddstSock( &vRet, iErrno, pcSock );
				}						
			}

			return( vRet );
		}			
				
		virtual void DelSock( T *pcSock )
		{
			typename vector<T *>::iterator p;
			
			for( unsigned int i = 0; i < size(); i++ )
			{
				if ( (*this)[i] == pcSock )
				{
					// clean up
					(*this)[i]->Disconnected();
					
					p = this->begin() + i;
					Zzap( (*this)[i] );
					this->erase( p );
					return;
				}
			}
			WARN( "WARNING!!! Could not find " + Cstring::num2Cstring( (unsigned int)pcSock ) );
		}

		//! internal use only
		virtual void AddstSock( vector<stSock> * pcvSt, EMessages eErrno, T * pcSock )
		{
			for( unsigned int i = 0; i < pcvSt->size(); i++ )
			{
				if ( (*pcvSt)[i].pcSock == pcSock )
					return;
			}
			stSock stPB;
			stPB.eErrno = eErrno;
			stPB.pcSock = pcSock;
			
			pcvSt->push_back( stPB );
		}

		//! these crons get ran and checked in Loop()
		virtual void Cron()
		{
			for( unsigned int a = 0; a < m_vcCrons.size(); a++ )
			{		
				CCron *pcCron = m_vcCrons[a];

				if ( !pcCron->isValid() )
				{
					Zzap( pcCron );
					m_vcCrons.erase( m_vcCrons.begin() + a-- );
				} else
					pcCron->run();
			}
		}

		//! only used internally
		virtual void DestroySock( T * pcClass ) { m_pcDestroySocks.push_back( pcClass ); }
		
		EMessages			m_errno;
		vector<T *>			m_pcDestroySocks;
		vector<CCron *>		m_vcCrons;
		unsigned long long	m_iCallTimeouts;	
		u_int				m_iSelectWait;
};

//! basic socket class
typedef TSocketManager<Csock> CSocketManager;

#endif /* _HAS_CSOCKET_ */

