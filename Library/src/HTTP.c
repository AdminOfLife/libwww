/*									 HTTP.c
**	MULTITHREADED IMPLEMENTATION OF HTTP CLIENT
**
**	(c) COPYRIGHT CERN 1994.
**	Please first read the full copyright statement in the file COPYRIGH.
**
**	This module implments the HTTP protocol as a state machine
**
** History:
**    < May 24 94 ??	Unknown - but obviously written
**	May 24 94 HF	Made reentrent and cleaned up a bit. Implemented
**			Forward, redirection, error handling and referer field
**	 8 Jul 94  FM	Insulate free() from _free structure element.
**	Jul 94 HFN	Written on top of HTTP.c, Henrik Frystyk
**
*/

/* Library include files */
#include "tcp.h"
#include "HTUtils.h"
#include "HTString.h"
#include "HTParse.h"
#include "HTTCP.h"
#include "HTFormat.h"
#include "HTAlert.h"
#include "HTMIME.h"
#include "HTAccess.h"		/* HTRequest */
#include "HTAABrow.h"		/* Access Authorization */
#include "HTTee.h"		/* Tee off a cache stream */
#include "HTFWrite.h"		/* Write to cache file */
#include "HTWriter.h"
#include "HTError.h"
#include "HTChunk.h"
#include "HTGuess.h"
#include "HTThread.h"
#include "HTTPReq.h"
#include "HTTP.h"					       /* Implements */

/* Macros and other defines */
#define PUTC(c)		(*me->target->isa->put_character)(me->target, c)
#define PUTS(s)		(*me->target->isa->put_string)(me->target, s)
#define PUTBLOCK(b, l)	(*me->target->isa->put_block)(me->target, b, l)
#define FREE_TARGET	(*me->target->isa->_free)(me->target)
#define ABORT_TARGET	(*me->target->isa->abort)(me->target, e)

/* Globals */
PUBLIC int  HTMaxRedirections = 10;	       /* Max number of redirections */

/* Type definitions and global variables etc. local to this module */
/* This is the local definition of HTRequest->net_info */
typedef enum _HTTPState {
    HTTP_ERROR		= -3,
    HTTP_NO_DATA	= -2,
    HTTP_GOT_DATA	= -1,
    HTTP_BEGIN		= 0,
    HTTP_NEED_CONNECTION,
    HTTP_NEED_REQUEST,
    HTTP_REDIRECTION,
    HTTP_AA
} HTTPState;

typedef struct _http_info {
    SOCKFD		sockfd;				/* Socket descripter */
    SockA 		sock_addr;		/* SockA is defined in tcp.h */
    HTInputSocket *	isoc;				     /* Input buffer */
    SocAction		action;			/* Result of the select call */
    HTStream *		target;				    /* Target stream */
    int 		addressCount;	     /* Attempts if multi-homed host */
    time_t		connecttime;		 /* Used on multihomed hosts */
    struct _HTRequest *	request;	   /* Link back to request structure */

    HTTPState		state;			  /* State of the connection */
} http_info;

#define MAX_STATUS_LEN		75    /* Max nb of chars to check StatusLine */

struct _HTStream {
    CONST HTStreamClass *	isa;
    HTStream *		  	target;
    HTRequest *			request;
    http_info *			http;
    HTSocketEOL			state;
    BOOL			transparent;
    double			version;		 /* @@@ DOESN'T WORK */
    int				status;
    char 			buffer[MAX_STATUS_LEN+1];
    int				buflen;
};

/* ------------------------------------------------------------------------- */
/* 			          Help Functions			     */
/* ------------------------------------------------------------------------- */

/*                                                                  HTTPCleanup
**
**      This function closes the connection and frees memory.
**
**      Returns 0 on OK, else -1
*/
PRIVATE int HTTPCleanup ARGS2(HTRequest *, req, BOOL, abort)
{
    http_info *http;
    int status = 0;
    if (!req || !req->net_info) {
	if (PROT_TRACE) fprintf(TDEST, "HTTPCleanup. Bad argument!\n");
	status = -1;
    } else {
	http = (http_info *) req->net_info;
	if (http->sockfd != INVSOC) {

	    /* Free stream with data TO network */
	    if (!req->PostCallBack) {
		if (abort)
		    (*req->input_stream->isa->abort)(req->input_stream, NULL);
		else
		    (*req->input_stream->isa->_free)(req->input_stream);
	    }
	    
	    /* Free stream with data FROM network */
	    if (abort)
		(*http->target->isa->abort)(http->target, NULL);
	    else
		(*http->target->isa->_free)(http->target);

	    if (PROT_TRACE)
		fprintf(TDEST,"HTTP........ Closing socket %d\n",http->sockfd);
	    if ((status = NETCLOSE(http->sockfd)) < 0)
		HTErrorSysAdd(http->request, ERR_FATAL, socerrno, NO,
			      "NETCLOSE");
	    HTThreadState(http->sockfd, THD_CLOSE);
	    http->sockfd = INVSOC;
	    HTThread_clear((HTNetInfo *) http);
	}
	if (http->isoc)
	    HTInputSocket_free(http->isoc);
	free(http);
	req->net_info = NULL;
    }	
    return status;
}


PRIVATE BOOL HTTPAuthentication ARGS1(HTRequest *, request)
{
    HTAAScheme scheme;
    HTList *valid_schemes = HTList_new();
    HTAssocList **scheme_specifics = NULL;
    char *tmplate = NULL;

    if (request->WWWAAScheme) {
	if ((scheme = HTAAScheme_enum(request->WWWAAScheme)) != HTAA_UNKNOWN) {
	    HTList_addObject(valid_schemes, (void *) scheme);
	    if (!scheme_specifics) {
		int i;
		scheme_specifics = (HTAssocList**)
		    malloc(HTAA_MAX_SCHEMES * sizeof(HTAssocList*));
		if (!scheme_specifics)
		    outofmem(__FILE__, "HTTPAuthentication");
		for (i=0; i < HTAA_MAX_SCHEMES; i++)
		    scheme_specifics[i] = NULL;
	    }
	    scheme_specifics[scheme] = HTAA_parseArgList(request->WWWAARealm);
	} else if (PROT_TRACE) {
	    HTErrorAdd(request, ERR_INFO, NO, HTERR_UNKNOWN_AA,
		       (void *) request->WWWAAScheme, 0, "HTTPAuthentication");
	    return NO;
	}
    }
    if (request->WWWprotection) {
	if (PROT_TRACE)
	    fprintf(TDEST, "Protection template set to `%s'\n",
		    request->WWWprotection);
	StrAllocCopy(tmplate, request->WWWprotection);
    }
    request->valid_schemes = valid_schemes;
    request->scheme_specifics = scheme_specifics;
    request->prot_template = tmplate;
    return YES;
}


/*
**	This is a big switch handling all HTTP return codes. It puts in any
**	appropiate error message and decides whether we should expect data
**	or not.
*/
PRIVATE void HTTPResponse ARGS1(HTStream *, me)
{
    switch (me->status) {

      case 0:						     /* 0.9 response */
      case 200:
      case 201:
      case 202:
      case 203:
      case 205:
      case 206:
	break;

      case 204:						      /* No Response */
	me->http->state = HTTP_NO_DATA;
	break;

      case 301:						   	    /* Moved */
      case 302:							    /* Found */
	me->http->state = HTTP_REDIRECTION;
	break;
	
      case 303:							   /* Method */
	HTAlert("This client doesn't support automatic redirection of type `Method'");
	me->http->state = HTTP_ERROR;
	break;
	
      case 400:						      /* Bad Request */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_BAD_REQUEST,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 401:
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_UNAUTHORIZED,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_AA;
	break;
	
      case 402:						 /* Payment required */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_PAYMENT_REQUIRED,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;
	
      case 403:							/* Forbidden */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_FORBIDDEN,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;
	
      case 404:							/* Not Found */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_NOT_FOUND,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;
	
      case 405:						      /* Not Allowed */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_NOT_ALLOWED,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 406:						  /* None Acceptable */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_NONE_ACCEPTABLE,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 407:			       	    /* Proxy Authentication Required */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_PROXY,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 408:						  /* Request Timeout */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_TIMEOUT,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 500:
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_INTERNAL,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;
	
      case 501:
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_NOT_IMPLEMENTED,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 502:
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_BAD_GATE,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 503:
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_DOWN,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      case 504:
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_GATE_TIMEOUT,
		   NULL, 0, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;

      default:						       /* bad number */
	HTErrorAdd(me->request, ERR_FATAL, NO, HTERR_BAD_REPLY,
		   (void *) me->buffer, me->buflen, "HTLoadHTTP");
	me->http->state = HTTP_ERROR;
	break;
    }
}

/* ------------------------------------------------------------------------- */
/* 			    HTTP Status Line Stream			     */
/* ------------------------------------------------------------------------- */

/*
**	Analyse the stream we have read. If it is a HTTP 1.0 or higher
**	then create a MIME-stream, else create a Guess stream to find out
**	what the 0.9 server is sending. We need to copy the buffer as we don't
**	know if we can modify the contents or not.
**
**	Stream handling is a function of the status code returned from the 
**	server:
**		200:	 Use `output_stream' in HTRequest structure
**		else:	 Use `error_stream' in HTRequest structure
**
**	Return: YES if buffer should be written out. NO otherwise
*/
PRIVATE int stream_pipe ARGS1(HTStream *, me)
{
    HTRequest *req = me->request;
    if (me->target) {
	int status = PUTBLOCK(me->buffer, me->buflen);
	if (status == HT_OK)
	    me->transparent = YES;
	return status;
    }
    if (strncasecomp(me->buffer, "http/", 5) ||
	sscanf(me->buffer+5, "%lf %d", &me->version, &me->status) < 2) {
	int status;
	HTErrorAdd(req, ERR_INFO, NO, HTERR_HTTP09,
		   (void *) me->buffer, me->buflen, "HTTPStatusStream");
	me->target = HTGuess_new(req, NULL, WWW_UNKNOWN,
				 req->output_format, req->output_stream);
	if ((status = PUTBLOCK(me->buffer, me->buflen)) == HT_OK)
	    me->transparent = YES;
	return status;
    } else {
	if (req->output_format == WWW_SOURCE) {
	    me->target = HTMIMEConvert(req, NULL, WWW_MIME, req->output_format,
				       req->output_stream);
	} else if (me->status==200) {
	    HTStream *s;
	    me->target = HTStreamStack(WWW_MIME, req->output_format,
				       req->output_stream, req, NO);
	    
	    /* howcome: test for return value from HTCacheWriter 12/1/95 */
	    if (HTCache_isEnabled() &&
		(s = HTCacheWriter(req, NULL, WWW_MIME,	req->output_format,
				   req->output_stream))) {
		me->target = HTTee(me->target, s);
	    }
	} else {
	    me->target = HTStreamStack(WWW_MIME, WWW_SOURCE,
				       req->error_stream ?
				       req->error_stream : HTBlackHole(),
				       req, NO);
	}
	if (!me->target)
	    me->target = HTBlackHole();				/* What else */
    }
    me->transparent = YES;
    return HT_OK;
}

/*
**	Searches for HTTP header line until buffer fills up or a CRLF or LF
**	is found
*/
PRIVATE int HTTPStatus_put_block ARGS3(HTStream *, me, CONST char*, b, int, l)
{
    while (!me->transparent && l-- > 0) {
	int status;
	if (me->target) {
	    if ((status = stream_pipe(me)) != HT_OK)
		return status;
	} else {
	    *(me->buffer+me->buflen++) = *b;
	    if (me->state == EOL_FCR) {
		if (*b == LF) {	/* Line found */
		    if ((status = stream_pipe(me)) != HT_OK)
			return status;
		} else {
		    me->state = EOL_BEGIN;
		}
	    } else if (*b == CR) {
		me->state = EOL_FCR;
	    } else if (*b == LF) {
		if ((status = stream_pipe(me)) != HT_OK)
		    return status;
	    } else {
		if (me->buflen >= MAX_STATUS_LEN) {
		    if ((status = stream_pipe(me)) != HT_OK)
			return status;
		}
	    }
	    b++;
	}
    }
    if (l > 0)
	return PUTBLOCK(b, l);
    return HT_OK;
}

PRIVATE int HTTPStatus_put_string ARGS2(HTStream *, me, CONST char*, s)
{
    return HTTPStatus_put_block(me, s, (int) strlen(s));
}

PRIVATE int HTTPStatus_put_character ARGS2(HTStream *, me, char, c)
{
    return HTTPStatus_put_block(me, &c, 1);
}

PRIVATE int HTTPStatus_flush ARGS1(HTStream *, me)
{
    return (*me->target->isa->flush)(me->target);
}

PRIVATE int HTTPStatus_free ARGS1(HTStream *, me)
{
    HTTPResponse(me);					   /* Get next state */
    if (me->target)
	FREE_TARGET;
    free(me);
    return HT_OK;
}

PRIVATE int HTTPStatus_abort ARGS2(HTStream *, me, HTError, e)
{
    if (me->target)
	ABORT_TARGET;
    free(me);
    if (PROT_TRACE)
	fprintf(TDEST, "HTTPStatus.. ABORTING...\n");
    return HT_ERROR;
}

/*	HTTPStatus Stream
**	-----------------
*/
PRIVATE CONST HTStreamClass HTTPStatusClass =
{		
    "HTTPStatus",
    HTTPStatus_flush,
    HTTPStatus_free,
    HTTPStatus_abort,
    HTTPStatus_put_character,
    HTTPStatus_put_string,
    HTTPStatus_put_block
};

PUBLIC HTStream * HTTPStatus_new ARGS2(HTRequest *, request,
				       http_info *, http)
{
    HTStream * me = (HTStream *) calloc(1, sizeof(HTStream));
    if (!me) outofmem(__FILE__, "HTTPStatus_new");
    me->isa = &HTTPStatusClass;
    me->request = request;
    me->http = http;
    me->state = EOL_BEGIN;
    return me;
}

/* ------------------------------------------------------------------------- */

/*		Load Document from HTTP Server			     HTLoadHTTP
**		==============================
**
**	Given a hypertext address, this routine loads a document.
**
** On entry,
**      request		This is the request structure
** On exit,
**	returns		HT_ERROR	Error has occured or interrupted
**			HT_WOULD_BLOCK  if operation would have blocked
**			HT_LOADED	if return status 200 OK
**			HT_NO_DATA	if return status 204 No Response
*/
PUBLIC int HTLoadHTTP ARGS1 (HTRequest *, request)
{
    int status = HT_ERROR;
    char *url;				  /* Gets initialized on every entry */
    http_info *http;			    /* Specific protocol information */

    if (!request || !request->anchor) {
        if (PROT_TRACE) fprintf(TDEST, "HTLoadHTTP.. Bad argument\n");
        return HT_ERROR;
    }
    url = HTAnchor_physical(request->anchor);
    
    /* Only do the setup first time through. This is actually state HTTP_BEGIN
       but it can't be in the state machine as we need the structure first */
    if (!request->net_info) {
	/*
	** Initiate a new http structure and bind to request structure
	** This is actually state HTTP_BEGIN, but it can't be in the state
	** machine as we need the structure first.
	*/
	if (PROT_TRACE) fprintf(TDEST, "HTTP........ Looking for `%s\'\n",url);
	if ((http = (http_info *) calloc(1, sizeof(http_info))) == NULL)
	    outofmem(__FILE__, "HTLoadHTTP");
	http->sockfd = INVSOC;			    /* Invalid socket number */
	http->request = request;
	http->state = HTTP_BEGIN;
	request->net_info = (HTNetInfo *) http;
	HTThread_new((HTNetInfo *) http);
	request->input_stream = HTTPRequest_new(request,request->input_stream);
    } else
	http = (http_info *) request->net_info;		/* Get existing copy */
 
    /* Now jump into the machine. We know the state from the previous run */
    while (1) {
	switch (http->state) {
	  case HTTP_BEGIN:
	    /*
	     ** Compose authorization information (this was moved here
	     ** from after the making of the connection so that the connection
	     ** wouldn't have to wait while prompting username and password
	     ** from the user).				-- AL 13.10.93
	     */
	    HTAA_composeAuth(request);
	    if (PROT_TRACE) {
		if (request->authorization)
		    fprintf(TDEST, "HTTP........ Sending Authorization: %s\n",
			    request->authorization);
		else
		    fprintf(TDEST,
			    "HTTP........ Not sending authorization (yet)\n");
	    }
	    http->state = HTTP_NEED_CONNECTION;
	    break;
	    
	  case HTTP_NEED_CONNECTION: 	    /* Now let's set up a connection */
	    status = HTDoConnect((HTNetInfo *) http, url, TCP_PORT,
				 NULL, NO);
	    if (status == HT_OK) {
		if (PROT_TRACE)
		    fprintf(TDEST, "HTTP........ Connected, socket %d\n",
			    http->sockfd);

		/* Set up read buffer, streams and concurrent read/write */
		http->isoc = HTInputSocket_new(http->sockfd);
		request->input_stream->target=HTWriter_new(http->sockfd, YES);
		http->target = HTImProxy ?
		    request->output_stream : HTTPStatus_new(request, http);
		HTThreadState(http->isoc->input_file_number, THD_SET_READ);
		http->state = HTTP_NEED_REQUEST;
	    } else if (status == HT_WOULD_BLOCK)
		return status;
	    else
		http->state = HTTP_ERROR;	       /* Error or interrupt */
	    break;

	    /* As we can do simultanous read and write this is now 1 state */
	  case HTTP_NEED_REQUEST:
	    if (http->action == SOC_WRITE) {

		/* Find the right way to call back */
		if (request->CopyRequest) {
		    if (!HTAnchor_headerParsed(request->CopyRequest->anchor))
			return HT_WOULD_BLOCK;
		    status = request->PostCallBack(request->CopyRequest,
						   request->input_stream);
		} else if (request->PostCallBack) {
		    status = request->PostCallBack(request,
						   request->input_stream);
		} else {
		    status = (*request->input_stream->isa->flush)
			(request->input_stream);
		}
		if (status == HT_WOULD_BLOCK)
		    return HT_WOULD_BLOCK;
		else if (status == HT_INTERRUPTED)
		    http->state = HTTP_ERROR;
		else
		    http->action = SOC_READ;
	    } else if (http->action == SOC_READ) {
		status = HTSocketRead(request, http->target);
		if (status == HT_WOULD_BLOCK)
		    return HT_WOULD_BLOCK;
		else if (status == HT_INTERRUPTED)
		    http->state = HTTP_ERROR;
		else if (status == HT_LOADED) {
		    if (http->state == HTTP_NEED_REQUEST)
			http->state = HTTP_GOT_DATA;
		} else
		    http->state = HTTP_ERROR;
	    } else
		http->state = HTTP_ERROR;
	    break;
	    
	  case HTTP_REDIRECTION:
	    if (request->redirect) {
		HTAnchor *anchor;
		if (status == 301) {
		    HTErrorAdd(request, ERR_INFO, NO, HTERR_MOVED,
			       (void *) request->redirect,
			       (int) strlen(request->redirect), "HTLoadHTTP");
		} else if (status == 302) {
		    HTErrorAdd(request, ERR_INFO, NO, HTERR_FOUND,
			       (void *) request->redirect,
			       (int) strlen(request->redirect), "HTLoadHTTP");
		}
		anchor = HTAnchor_findAddress(request->redirect);
		if (++request->redirections < HTMaxRedirections) {
		    HTTPCleanup(request, NO);
		    return HTLoadAnchorRecursive((HTAnchor *) anchor, request);
		} else {
		    HTErrorAdd(request, ERR_FATAL, NO, HTERR_MAX_REDIRECT,
			       NULL, 0, "HTLoadHTTP");
		    http->state = HTTP_ERROR;
		}
	    } else {
		HTErrorAdd(request, ERR_FATAL, NO, HTERR_BAD_REPLY,
			   NULL, 0, "HTLoadHTTP");
		http->state = HTTP_ERROR;
	    }
	    break;
	    
	  case HTTP_AA:
	    HTTPCleanup(request, NO);			 /* Close connection */
	    if (HTTPAuthentication(request) == YES &&
		HTAA_retryWithAuth(request) == YES) {
		return HTLoadAnchor((HTAnchor *) request->anchor, request);
	    } else {
		char *unescaped = NULL;
		StrAllocCopy(unescaped, url);
		HTUnEscape(unescaped);
		HTErrorAdd(request, ERR_FATAL, NO, HTERR_UNAUTHORIZED,
			   (void *) unescaped,
			   (int) strlen(unescaped), "HTLoadHTTP");
		free(unescaped);
		return HT_ERROR;
	    }
	    break;
	    
	  case HTTP_GOT_DATA:
	    HTTPCleanup(request, NO);
	    return HT_LOADED;
	    break;
	    
	  case HTTP_NO_DATA:
	    HTTPCleanup(request, NO);
	    return HT_NO_DATA;
	    break;
	    
	  case HTTP_ERROR:
	    HTTPCleanup(request, YES);
	    return HT_ERROR;
	    break;
	}
    } /* End of while(1) */
}    

/* Protocol descriptor */

GLOBALDEF PUBLIC HTProtocol HTTP = {
    "http", SOC_NON_BLOCK, HTLoadHTTP, NULL, NULL
};

