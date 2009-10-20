/* $Id$ */
/* 
 * Copyright (C) 2009 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#include <pj/ssl_sock.h>
#include <pj/activesock.h>
#include <pj/compat/socket.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/list.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/math.h>
#include <pj/pool.h>
#include <pj/string.h>


/* Only build when PJ_HAS_SSL_SOCK is enabled */
//#if defined(PJ_HAS_SSL_SOCK) && PJ_HAS_SSL_SOCK!=0

#define THIS_FILE		"ssl_sock_ossl.c"

/* 
 * Include OpenSSL headers 
 */
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>


#ifdef _MSC_VER
# ifdef _DEBUG
#  pragma comment( lib, "libeay32MTd")
#  pragma comment( lib, "ssleay32MTd")
#else
#  pragma comment( lib, "libeay32MT")
#  pragma comment( lib, "ssleay32MT")
# endif
#endif


/*
 * SSL/TLS state enumeration.
 */
enum ssl_state {
    SSL_STATE_NULL,
    SSL_STATE_HANDSHAKING,
    SSL_STATE_ESTABLISHED
};

/*
 * Structure of SSL socket read buffer.
 */
typedef struct read_data_t
{
    void		 *data;
    pj_size_t		  len;
} read_data_t;

/*
 * Get the offset of pointer to read-buffer of SSL socket from read-buffer
 * of active socket. Note that both SSL socket and active socket employ 
 * different but correlated read-buffers (as much as async_cnt for each),
 * and to make it easier/faster to find corresponding SSL socket's read-buffer
 * from known active socket's read-buffer, the pointer of corresponding 
 * SSL socket's read-buffer is stored right after the end of active socket's
 * read-buffer.
 */
#define OFFSET_OF_READ_DATA_PTR(ssock, asock_rbuf) \
					(read_data_t**) \
					((pj_int8_t*)(asock_rbuf) + \
					ssock->param.read_buffer_size)

/*
 * Structure of SSL socket write buffer.
 */
typedef struct write_data_t {
    pj_ioqueue_op_key_t	 key;
    pj_size_t 	 	 record_len;
    pj_ioqueue_op_key_t	*app_key;
    pj_size_t 	 	 plain_data_len;
    pj_size_t 	 	 data_len;
    union {
	char		 content[1];
	const char	*ptr;
    } data;
    unsigned		 flags;
} write_data_t;

/*
 * Structure of SSL socket write state.
 */
typedef struct write_state_t {
    char		*buf;
    pj_size_t		 max_len;    
    char		*start;
    pj_size_t		 len;
    write_data_t	*last_data;
} write_state_t;

/*
 * Structure of write data pending.
 */
typedef struct write_pending_t {
    PJ_DECL_LIST_MEMBER(struct write_pending_t);
    write_data_t	 data;
} write_pending_t;

/*
 * Secure socket structure definition.
 */
struct pj_ssl_sock_t
{
    pj_pool_t		 *pool;
    pj_ssl_sock_t	 *parent;
    pj_ssl_sock_param	  param;
    pj_ssl_cert_t	 *cert;

    pj_bool_t		  is_server;
    enum ssl_state	  ssl_state;
    pj_ioqueue_op_key_t	  handshake_op_key;

    pj_sock_t		  sock;
    pj_activesock_t	 *asock;

    pj_sockaddr		  local_addr;
    pj_sockaddr		  rem_addr;
    int			  addr_len;
    
    pj_bool_t		  read_started;
    pj_size_t		  read_size;
    pj_uint32_t		  read_flags;
    void		**asock_rbuf;
    read_data_t		 *ssock_rbuf;

    write_state_t	  write_state;
    write_pending_t	  write_pending;
    write_pending_t	  write_pending_empty;
    pj_lock_t		 *write_mutex; /* protect write BIO and write_state */

    SSL_CTX		 *ossl_ctx;
    SSL			 *ossl_ssl;
    BIO			 *ossl_rbio;
    BIO			 *ossl_wbio;
};


/*
 * Certificate/credential structure definition.
 */
struct pj_ssl_cert_t
{
    pj_str_t CA_file;
    pj_str_t cert_file;
    pj_str_t privkey_file;
    pj_str_t privkey_pass;
};


static pj_status_t flush_delayed_send(pj_ssl_sock_t *ssock);

/*
 *******************************************************************
 * Static/internal functions.
 *******************************************************************
 */

/* ssl_report_error() */
static void ssl_report_error(const char *sender, int level, 
			     pj_status_t status,
			     const char *format, ...)
{
    va_list marker;

    va_start(marker, format);

#if PJ_LOG_MAX_LEVEL > 0
    if (status != PJ_SUCCESS) {
	char err_format[PJ_ERR_MSG_SIZE + 512];
	int len;

	len = pj_ansi_snprintf(err_format, sizeof(err_format),
			       "%s: ", format);
	pj_strerror(status, err_format+len, sizeof(err_format)-len);
	
	pj_log(sender, level, err_format, marker);

    } else {
	unsigned long ssl_err;

	ssl_err = ERR_get_error();

	if (ssl_err == 0) {
	    pj_log(sender, level, format, marker);
	} else {
	    char err_format[512];
	    int len;

	    len = pj_ansi_snprintf(err_format, sizeof(err_format),
				   "%s: ", format);
	    ERR_error_string(ssl_err, err_format+len);
	    
	    pj_log(sender, level, err_format, marker);
	}
    }
#endif

    va_end(marker);
}



/* OpenSSL library initialization counter */
static int openssl_init_count;

/* OpenSSL available ciphers */
static pj_ssl_cipher openssl_ciphers[64];
static unsigned openssl_cipher_num;


/* Initialize OpenSSL */
static pj_status_t init_openssl(void)
{
    if (++openssl_init_count != 1)
	return PJ_SUCCESS;

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    /* Init available ciphers */
    if (openssl_cipher_num == 0) {
	SSL_METHOD *meth = NULL;
	SSL_CTX *ctx;
	SSL *ssl;
	STACK_OF(SSL_CIPHER) *sk_cipher;
	unsigned i, n;

	meth = (SSL_METHOD*)SSLv23_server_method();
	if (!meth)
	    meth = (SSL_METHOD*)TLSv1_server_method();
	if (!meth)
	    meth = (SSL_METHOD*)SSLv3_server_method();
	if (!meth)
	    meth = (SSL_METHOD*)SSLv2_server_method();
	pj_assert(meth);

	ctx=SSL_CTX_new(meth);
	SSL_CTX_set_cipher_list(ctx, "ALL");

	ssl = SSL_new(ctx);
	sk_cipher = SSL_get_ciphers(ssl);

	n = sk_SSL_CIPHER_num(sk_cipher);
	if (n > PJ_ARRAY_SIZE(openssl_ciphers))
	    n = PJ_ARRAY_SIZE(openssl_ciphers);

	for (i = 0; i < n; ++i) {
	    SSL_CIPHER *c;
	    c = sk_SSL_CIPHER_value(sk_cipher,i);
	    openssl_ciphers[i] = (pj_ssl_cipher)
				 (pj_uint32_t)c->id & 0x00FFFFFF;
	    //printf("%3u: %08x=%s\n", i+1, c->id, SSL_CIPHER_get_name(c));
	}

	SSL_free(ssl);
	SSL_CTX_free(ctx);

	openssl_cipher_num = n;
    }

    return PJ_SUCCESS;
}


/* Shutdown OpenSSL */
static void shutdown_openssl(void)
{
    if (--openssl_init_count != 0)
	return;
}


/* SSL password callback. */
static int password_cb(char *buf, int num, int rwflag, void *user_data)
{
    pj_ssl_cert_t *cert = (pj_ssl_cert_t*) user_data;

    PJ_UNUSED_ARG(rwflag);

    if(num < cert->privkey_pass.slen)
	return 0;
    
    pj_memcpy(buf, cert->privkey_pass.ptr, cert->privkey_pass.slen);
    return cert->privkey_pass.slen;
}


/* Create and initialize new SSL context */
static pj_status_t create_ssl_ctx(pj_ssl_sock_t *ssock, SSL_CTX **p_ctx)
{
    SSL_METHOD *ssl_method;
    SSL_CTX *ctx;
    pj_ssl_cert_t *cert;
    int mode, rc;
        
    pj_assert(ssock && p_ctx);

    cert = ssock->cert;

    /* Make sure OpenSSL library has been initialized */
    init_openssl();

    /* Determine SSL method to use */
    switch (ssock->param.proto) {
    case PJ_SSL_SOCK_PROTO_DEFAULT:
    case PJ_SSL_SOCK_PROTO_TLS1:
	ssl_method = (SSL_METHOD*)TLSv1_method();
	break;
    case PJ_SSL_SOCK_PROTO_SSL2:
	ssl_method = (SSL_METHOD*)SSLv2_method();
	break;
    case PJ_SSL_SOCK_PROTO_SSL3:
	ssl_method = (SSL_METHOD*)SSLv3_method();
	break;
    case PJ_SSL_SOCK_PROTO_SSL23:
	ssl_method = (SSL_METHOD*)SSLv23_method();
	break;
    case PJ_SSL_SOCK_PROTO_DTLS1:
	ssl_method = (SSL_METHOD*)DTLSv1_method();
	break;
    default:
	ssl_report_error(THIS_FILE, 4, PJ_EINVAL,
			 "Error creating SSL context");
	return PJ_EINVAL;
    }

    /* Create SSL context for the listener */
    ctx = SSL_CTX_new(ssl_method);
    if (ctx == NULL) {
	ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
			 "Error creating SSL context");
	return PJ_EINVAL;
    }

    /* Apply credentials */
    if (cert) {
	/* Load CA list if one is specified. */
	if (cert->CA_file.slen) {

	    rc = SSL_CTX_load_verify_locations(ctx, cert->CA_file.ptr, NULL);

	    if (rc != 1) {
		ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
				 "Error loading/verifying CA list file '%s'",
				 cert->CA_file.ptr);
		SSL_CTX_free(ctx);
		return PJ_EINVAL;
	    }

	    PJ_LOG(5,(THIS_FILE, "CA file successfully loaded from '%s'",
		      cert->CA_file.ptr));
	}
    
	/* Set password callback */
	if (cert->privkey_pass.slen) {
	    SSL_CTX_set_default_passwd_cb(ctx, password_cb);
	    SSL_CTX_set_default_passwd_cb_userdata(ctx, cert);
	}


	/* Load certificate if one is specified */
	if (cert->cert_file.slen) {

	    /* Load certificate chain from file into ctx */
	    rc = SSL_CTX_use_certificate_chain_file(ctx, cert->cert_file.ptr);

	    if(rc != 1) {
		ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
				 "Error loading certificate chain file '%s'",
				 cert->cert_file.ptr);
		SSL_CTX_free(ctx);
		return PJ_EINVAL;
	    }

	    PJ_LOG(5,(THIS_FILE, "TLS certificate successfully loaded from '%s'",
		      cert->cert_file.ptr));
	}


	/* Load private key if one is specified */
	if (cert->privkey_file.slen) {
	    /* Adds the first private key found in file to ctx */
	    rc = SSL_CTX_use_PrivateKey_file(ctx, cert->privkey_file.ptr, 
					     SSL_FILETYPE_PEM);

	    if(rc != 1) {
		ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
				 "Error adding private key from '%s'",
				 cert->privkey_file.ptr);
		SSL_CTX_free(ctx);
		return PJ_EINVAL;
	    }

	    PJ_LOG(5,(THIS_FILE, "Private key successfully loaded from '%s'",
		      cert->privkey_file.ptr));
	}
    }


    /* SSL verification options */
    if (ssock->param.verify_peer) {
	mode = SSL_VERIFY_PEER;
    } else {
	mode = SSL_VERIFY_NONE;
    }

    if (ssock->is_server && ssock->param.require_client_cert)
	mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

    SSL_CTX_set_verify(ctx, mode, NULL);

    PJ_LOG(5,(THIS_FILE, "Verification mode set to %d", mode));

    *p_ctx = ctx;

    return PJ_SUCCESS;
}


/* Destroy SSL context */
static void destroy_ssl_ctx(SSL_CTX *ctx)
{
    SSL_CTX_free(ctx);

    /* Potentially shutdown OpenSSL library if this is the last
     * context exists.
     */
    shutdown_openssl();
}


/* Reset SSL socket state */
static void reset_ssl_sock_state(pj_ssl_sock_t *ssock)
{
    ssock->ssl_state = SSL_STATE_NULL;

    if (ssock->ossl_ssl) {
	SSL_shutdown(ssock->ossl_ssl);
	SSL_free(ssock->ossl_ssl); /* this will also close BIOs */
	ssock->ossl_ssl = NULL;
    }
    if (ssock->ossl_ctx) {
	destroy_ssl_ctx(ssock->ossl_ctx);
	ssock->ossl_ctx = NULL;
    }
    if (ssock->asock) {
	pj_activesock_close(ssock->asock);
	ssock->asock = NULL;
	ssock->sock = PJ_INVALID_SOCKET;
    }
}


/* Generate cipher list with user preference order in OpenSSL format */
static pj_status_t set_cipher_list(pj_ssl_sock_t *ssock)
{
    char buf[1024];
    pj_str_t cipher_list;
    STACK_OF(SSL_CIPHER) *sk_cipher;
    unsigned i;
    int j, ret;

    if (ssock->param.ciphers_num == 0)
	return PJ_SUCCESS;

    pj_strset(&cipher_list, buf, 0);

    /* Set SSL with ALL available ciphers */
    SSL_set_cipher_list(ssock->ossl_ssl, "ALL");

    /* Generate user specified cipher list in OpenSSL format */
    sk_cipher = SSL_get_ciphers(ssock->ossl_ssl);
    for (i = 0; i < ssock->param.ciphers_num; ++i) {
	for (j = 0; j < sk_SSL_CIPHER_num(sk_cipher); ++j) {
	    SSL_CIPHER *c;
	    c = sk_SSL_CIPHER_value(sk_cipher, j);
	    if (ssock->param.ciphers[i] == (pj_ssl_cipher)
					   ((pj_uint32_t)c->id & 0x00FFFFFF))
	    {
		const char *c_name;

		c_name = SSL_CIPHER_get_name(c);

		/* Check buffer size */
		if (cipher_list.slen + pj_ansi_strlen(c_name) + 2 > sizeof(buf)) {
		    pj_assert(!"Insufficient temporary buffer for cipher");
		    return PJ_ETOOMANY;
		}

		/* Add colon separator */
		if (cipher_list.slen)
		    pj_strcat2(&cipher_list, ":");

		/* Add the cipher */
		pj_strcat2(&cipher_list, c_name);
		break;
	    }
	}
    }

    /* Put NULL termination in the generated cipher list */
    cipher_list.ptr[cipher_list.slen] = '\0';

    /* Finally, set chosen cipher list */
    ret = SSL_set_cipher_list(ssock->ossl_ssl, buf);
    if (ret < 1) {
	ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
			 "Error setting cipher list");
	return PJ_EINVAL;
    }

    return PJ_SUCCESS;
}


/* When handshake completed:
 * - notify application
 * - if handshake failed, reset SSL state
 * - return PJ_FALSE when SSL socket instance is destroyed by application.
 */
static pj_bool_t on_handshake_complete(pj_ssl_sock_t *ssock, 
				       pj_status_t status)
{
    /* Accepting */
    if (ssock->is_server) {
	if (status != PJ_SUCCESS) {
	    /* Handshake failed in accepting, destroy our self silently. */
	    pj_ssl_sock_close(ssock);
	    return PJ_FALSE;
	}
	/* Notify application the newly accepted SSL socket */
	if (ssock->param.cb.on_accept_complete) {
	    pj_bool_t ret;
	    ret = (*ssock->param.cb.on_accept_complete)
		      (ssock->parent, ssock, (pj_sockaddr_t*)&ssock->rem_addr,
		       pj_sockaddr_get_len((pj_sockaddr_t*)&ssock->rem_addr));
	    if (ret == PJ_FALSE)
		return PJ_FALSE;
	}
    }

    /* Connecting */
    else {
	if (ssock->param.cb.on_connect_complete) {
	    pj_bool_t ret;
	    ret = (*ssock->param.cb.on_connect_complete)(ssock, status);
	    if (ret == PJ_FALSE)
		return PJ_FALSE;
	}
	if (status != PJ_SUCCESS) {
	    /* Reset SSL socket state */
	    reset_ssl_sock_state(ssock);
	}
    }

    return PJ_TRUE;
}

/* Flush write BIO to network socket. Note that any access to write BIO
 * MUST be serialized, so mutex protection must cover any call to OpenSSL
 * API (that possibly generate data for write BIO) along with the call to
 * this function (flushing all data in write BIO generated by above 
 * OpenSSL API call).
 */
static pj_status_t flush_write_bio(pj_ssl_sock_t *ssock, 
				   pj_ioqueue_op_key_t *send_key,
				   pj_size_t orig_len,
				   unsigned flags)
{
    char *data;
    pj_ssize_t len;

    write_state_t *write_st = &ssock->write_state;
    write_data_t *wdata;
    pj_size_t avail_len, needed_len, skipped_len = 0;
    pj_status_t status;

    /* Check if there is data in write BIO, flush it if any */
    if (!BIO_pending(ssock->ossl_wbio))
	return PJ_SUCCESS;

    /* Get data and its length */
    len = BIO_get_mem_data(ssock->ossl_wbio, &data);
    if (len == 0)
	return PJ_SUCCESS;

    /* Calculate buffer size needed, and align it to 8 */
    needed_len = len + sizeof(write_data_t);
    needed_len = ((needed_len + 7) >> 3) << 3;

    /* Check buffer availability */
    avail_len = write_st->max_len - write_st->len;
    if (avail_len < needed_len)
	return PJ_ENOMEM;

    /* More buffer availability check, note that the write data must be in
     * a contigue buffer.
     */
    if (write_st->len == 0) {

	write_st->start = write_st->buf;
	wdata = (write_data_t*)write_st->start;

    } else {

	char *reg1, *reg2;
	pj_size_t reg1_len, reg2_len;

	/* Unused slots may be wrapped/splitted into two regions, so let's
	 * analyze them if any region can hold the write data.
	 */
	reg1 = write_st->start + write_st->len;
	if (reg1 >= write_st->buf + write_st->max_len)
	    reg1 -= write_st->max_len;
	reg1_len = write_st->max_len - write_st->len;
	if (reg1 + reg1_len > write_st->buf + write_st->max_len) {
	    reg1_len = write_st->buf + write_st->max_len - reg1;
	    reg2 = write_st->buf;
	    reg2_len = write_st->start - write_st->buf;
	} else {
	    reg2 = NULL;
	    reg2_len = 0;
	}
	avail_len = PJ_MAX(reg1_len, reg2_len);
	if (avail_len < needed_len)
	    return PJ_ENOMEM;

	/* Get write data pointer and update buffer length */
	if (reg1_len >= needed_len) {
	    wdata = (write_data_t*)reg1;
	} else {
	    wdata = (write_data_t*)reg2;
	    /* Unused slot in region 1 is skipped as current write data
	     * doesn't fit it.
	     */
	    skipped_len = reg1_len;
	}
    }

    /* Copy the data and set its properties into the buffer */
    pj_bzero(wdata, sizeof(write_data_t));
    wdata->app_key = send_key;
    wdata->record_len = needed_len;
    wdata->data_len = len;
    wdata->plain_data_len = orig_len;
    wdata->flags = flags;
    pj_memcpy(&wdata->data, data, len);

    /* Send it */
    if (ssock->param.sock_type == pj_SOCK_STREAM()) {
	status = pj_activesock_send(ssock->asock, &wdata->key, 
				    wdata->data.content, &len,
				    flags);
    } else {
	status = pj_activesock_sendto(ssock->asock, &wdata->key, 
				      wdata->data.content, &len,
				      flags,
				      (pj_sockaddr_t*)&ssock->rem_addr,
				      ssock->addr_len);
    }

    /* Oh no, EWOULDBLOCK! */
    if (status == PJ_STATUS_FROM_OS(OSERR_EWOULDBLOCK)) {
	/* Just return PJ_SUCCESS here, the pending data will be sent in next
	 * call of this function since the data is still stored in write BIO.
	 */
	return PJ_SUCCESS;
    }

    /* Reset write BIO after flushed */
    BIO_reset(ssock->ossl_wbio);

    if (status == PJ_EPENDING) {
	/* Update write state */
	pj_assert(skipped_len==0 || write_st->last_data);
	write_st->len += needed_len + skipped_len;
	if (write_st->last_data)
	    write_st->last_data->record_len += skipped_len;
	write_st->last_data = wdata;
    }

    return status;
}

static pj_status_t do_handshake(pj_ssl_sock_t *ssock)
{
    pj_status_t status;
    int err;

    pj_lock_acquire(ssock->write_mutex);

    /* Perform SSL handshake */
    err = SSL_do_handshake(ssock->ossl_ssl);
    if (err < 0) {
	err = SSL_get_error(ssock->ossl_ssl, err);
	if (err != SSL_ERROR_NONE && err != SSL_ERROR_WANT_READ) 
	{
	    /* Handshake fails */
	    ssl_report_error(THIS_FILE, 4, PJ_SUCCESS, 
			     "SSL_do_handshake()");
	    pj_lock_release(ssock->write_mutex);
	    return PJ_ECANCELLED;
	}
    }

    /* SSL_do_handshake() may put some pending data into SSL write BIO, 
     * flush it if any.
     */
    status = flush_write_bio(ssock, &ssock->handshake_op_key, 0, 0);
    if (status != PJ_SUCCESS && status != PJ_EPENDING) {
	pj_lock_release(ssock->write_mutex);
	return status;
    }

    pj_lock_release(ssock->write_mutex);

    /* Check if handshake has been completed */
    if (SSL_is_init_finished(ssock->ossl_ssl)) {
	ssock->ssl_state = SSL_STATE_ESTABLISHED;
	return PJ_SUCCESS;
    }

    return PJ_EPENDING;
}


/*
 *******************************************************************
 * Active socket callbacks.
 *******************************************************************
 */

static pj_bool_t asock_on_data_read (pj_activesock_t *asock,
				     void *data,
				     pj_size_t size,
				     pj_status_t status,
				     pj_size_t *remainder)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)
			   pj_activesock_get_user_data(asock);
    pj_size_t nwritten;

    /* Socket error or closed */
    if (data == NULL || size < 0)
	goto on_error;

    /* Consume the whole data */
    nwritten = BIO_write(ssock->ossl_rbio, data, size);
    if (nwritten < size) {
	status = PJ_ENOMEM;
	ssl_report_error(THIS_FILE, 4, PJ_SUCCESS, "BIO_write()");
	goto on_error;
    }

    /* Check if SSL handshake hasn't finished yet */
    if (ssock->ssl_state == SSL_STATE_HANDSHAKING) {
	pj_bool_t ret = PJ_TRUE;

	if (status == PJ_SUCCESS)
	    status = do_handshake(ssock);

	/* Not pending is either success or failed */
	if (status != PJ_EPENDING)
	    ret = on_handshake_complete(ssock, status);

	return ret;
    }

    /* See if there is any decrypted data for the application */
    if (ssock->read_started) {
	do {
	    read_data_t *buf = *(OFFSET_OF_READ_DATA_PTR(ssock, data));
	    void *data_ = (pj_int8_t*)buf->data + buf->len;
	    int size_ = ssock->read_size - buf->len;

	    /* SSL_read() may write some data to BIO write when re-negotiation
	     * is on progress, so let's protect it with write mutex.
	     */
	    pj_lock_acquire(ssock->write_mutex);

	    size_ = SSL_read(ssock->ossl_ssl, data_, size_);
	    if (size_ > 0) {
		pj_lock_release(ssock->write_mutex);
		if (ssock->param.cb.on_data_read) {
		    pj_bool_t ret;
		    pj_size_t remainder_ = 0;

		    buf->len += size_;
    		
		    ret = (*ssock->param.cb.on_data_read)(ssock, buf->data,
							  buf->len, status,
							  &remainder_);
		    if (!ret) {
			/* We've been destroyed */
			return PJ_FALSE;
		    }

		    /* Application may have left some data to be consumed 
		     * later.
		     */
		    buf->len = remainder_;
		}
	    } else {
		int err = SSL_get_error(ssock->ossl_ssl, size);
		
		/* SSL might just return SSL_ERROR_WANT_READ in 
		 * re-negotiation.
		 */
		if (err != SSL_ERROR_NONE && err != SSL_ERROR_WANT_READ)
		{
		    /* Reset SSL socket state, then return PJ_FALSE */
		    ssl_report_error(THIS_FILE, 4, PJ_SUCCESS, "SSL_read()");
		    pj_lock_release(ssock->write_mutex);
		    reset_ssl_sock_state(ssock);
		    return PJ_FALSE;
		}

		/* SSL may write something in case of re-negotiation */
		status = flush_write_bio(ssock, &ssock->handshake_op_key, 0, 0);
		pj_lock_release(ssock->write_mutex);
		if (status != PJ_SUCCESS && status != PJ_EPENDING)
		    goto on_error;

		/* If re-negotiation has been completed, start flushing
		 * delayed send.
		 */
		if (!SSL_renegotiate_pending(ssock->ossl_ssl)) {
		    status = flush_delayed_send(ssock);
		    if (status != PJ_SUCCESS && status != PJ_EPENDING)
			goto on_error;
		}

		break;
	    }
	} while (1);
    }

    return PJ_TRUE;

on_error:
    if (ssock->ssl_state == SSL_STATE_HANDSHAKING)
	return on_handshake_complete(ssock, status);

    if (ssock->read_started && ssock->param.cb.on_data_read) {
	pj_bool_t ret;
	ret = (*ssock->param.cb.on_data_read)(ssock, NULL, 0, status,
					      remainder);
	if (!ret) {
	    /* We've been destroyed */
	    return PJ_FALSE;
	}
    }

    reset_ssl_sock_state(ssock);
    return PJ_FALSE;
}


static pj_bool_t asock_on_data_sent (pj_activesock_t *asock,
				     pj_ioqueue_op_key_t *send_key,
				     pj_ssize_t sent)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)
			   pj_activesock_get_user_data(asock);

    PJ_UNUSED_ARG(send_key);
    PJ_UNUSED_ARG(sent);

    if (ssock->ssl_state == SSL_STATE_HANDSHAKING) {
	/* Initial handshaking */
	pj_status_t status;
	
	status = do_handshake(ssock);
	/* Not pending is either success or failed */
	if (status != PJ_EPENDING)
	    return on_handshake_complete(ssock, status);

    } else if (send_key != &ssock->handshake_op_key) {
	/* Some data has been sent, notify application */
	write_data_t *wdata = (write_data_t*)send_key;
	if (ssock->param.cb.on_data_sent) {
	    pj_bool_t ret;
	    ret = (*ssock->param.cb.on_data_sent)(ssock, wdata->app_key, 
						  wdata->plain_data_len);
	    if (!ret) {
		/* We've been destroyed */
		return PJ_FALSE;
	    }
	}

	/* Update write buffer state */
	pj_lock_acquire(ssock->write_mutex);
	ssock->write_state.start += wdata->record_len;
	ssock->write_state.len -= wdata->record_len;
	if (ssock->write_state.last_data == wdata) {
	    pj_assert(ssock->write_state.len == 0);
	    ssock->write_state.last_data = NULL;
	}
	pj_lock_release(ssock->write_mutex);

    } else {
	/* SSL re-negotiation is on-progress, just do nothing */
    }

    return PJ_TRUE;
}


static pj_bool_t asock_on_accept_complete (pj_activesock_t *asock,
					   pj_sock_t newsock,
					   const pj_sockaddr_t *src_addr,
					   int src_addr_len)
{
    pj_ssl_sock_t *ssock_parent = (pj_ssl_sock_t*)
				  pj_activesock_get_user_data(asock);
    pj_ssl_sock_t *ssock;
    pj_activesock_cb asock_cb;
    pj_activesock_cfg asock_cfg;
    unsigned i;
    pj_status_t status;

    PJ_UNUSED_ARG(src_addr_len);

    /* Create new SSL socket instance */
    status = pj_ssl_sock_create(ssock_parent->pool, &ssock_parent->param,
				&ssock);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Update new SSL socket attributes */
    ssock->sock = newsock;
    ssock->parent = ssock_parent;
    ssock->is_server = PJ_TRUE;
    if (ssock_parent->cert) {
	status = pj_ssl_sock_set_certificate(ssock, ssock->pool, 
					     ssock_parent->cert);
	if (status != PJ_SUCCESS)
	    goto on_return;
    }

    /* Update local address */
    ssock->addr_len = src_addr_len;
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr, 
				 &ssock->addr_len);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Set remote address */
    pj_sockaddr_cp(&ssock->rem_addr, src_addr);

    /* Create SSL context */
    status = create_ssl_ctx(ssock, &ssock->ossl_ctx);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Create SSL instance */
    ssock->ossl_ssl = SSL_new(ssock->ossl_ctx);
    if (ssock->ossl_ssl == NULL) {
	ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
			 "Error creating SSL connection object");
	status = PJ_EINVAL;
	goto on_return;
    }

    /* Set cipher list */
    status = set_cipher_list(ssock);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Setup SSL BIOs */
    ssock->ossl_rbio = BIO_new(BIO_s_mem());
    ssock->ossl_wbio = BIO_new(BIO_s_mem());
    BIO_set_close(ssock->ossl_rbio, BIO_CLOSE);
    BIO_set_close(ssock->ossl_wbio, BIO_CLOSE);
    SSL_set_bio(ssock->ossl_ssl, ssock->ossl_rbio, ssock->ossl_wbio);

    /* Prepare read buffer */
    ssock->asock_rbuf = (void**)pj_pool_calloc(ssock->pool, 
					       ssock->param.async_cnt,
					       sizeof(void*));
    for (i = 0; i<ssock->param.async_cnt; ++i) {
	ssock->asock_rbuf[i] = (void*) pj_pool_alloc(
					    ssock->pool, 
					    ssock->param.read_buffer_size + 
					    sizeof(read_data_t*));
    }

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = ssock->param.async_cnt;
    asock_cfg.concurrency = ssock->param.concurrency;
    asock_cfg.whole_data = PJ_TRUE;

    pj_bzero(&asock_cb, sizeof(asock_cb));
    asock_cb.on_data_read = asock_on_data_read;
    asock_cb.on_data_sent = asock_on_data_sent;

    status = pj_activesock_create(ssock->pool,
				  ssock->sock, 
				  ssock->param.sock_type,
				  &asock_cfg,
				  ssock->param.ioqueue, 
				  &asock_cb,
				  ssock,
				  &ssock->asock);

    if (status != PJ_SUCCESS)
	goto on_return;

    /* Start read */
    status = pj_activesock_start_read2(ssock->asock, ssock->pool, 
				       ssock->param.read_buffer_size,
				       ssock->asock_rbuf,
				       PJ_IOQUEUE_ALWAYS_ASYNC);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Prepare write/send state */
    pj_assert(ssock->write_state.max_len == 0);
    ssock->write_state.buf = (char*)
			     pj_pool_alloc(ssock->pool, 
					   ssock->param.send_buffer_size);
    ssock->write_state.max_len = ssock->param.send_buffer_size;
    ssock->write_state.start = ssock->write_state.buf;
    ssock->write_state.len = 0;

    /* Start SSL handshake */
    ssock->ssl_state = SSL_STATE_HANDSHAKING;
    SSL_set_accept_state(ssock->ossl_ssl);
    status = do_handshake(ssock);
    if (status != PJ_EPENDING)
	goto on_return;

    return PJ_TRUE;

on_return:
    return on_handshake_complete(ssock, status);
}


static pj_bool_t asock_on_connect_complete (pj_activesock_t *asock,
					    pj_status_t status)
{
    pj_ssl_sock_t *ssock = (pj_ssl_sock_t*)
			   pj_activesock_get_user_data(asock);
    unsigned i;

    if (status != PJ_SUCCESS)
	goto on_return;

    /* Update local address */
    ssock->addr_len = sizeof(pj_sockaddr);
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr, 
				 &ssock->addr_len);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Create SSL context */
    status = create_ssl_ctx(ssock, &ssock->ossl_ctx);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Create SSL instance */
    ssock->ossl_ssl = SSL_new(ssock->ossl_ctx);
    if (ssock->ossl_ssl == NULL) {
	ssl_report_error(THIS_FILE, 4, PJ_SUCCESS,
			 "Error creating SSL connection object");
	status = PJ_EINVAL;
	goto on_return;
    }

    /* Set cipher list */
    status = set_cipher_list(ssock);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Setup SSL BIOs */
    ssock->ossl_rbio = BIO_new(BIO_s_mem());
    ssock->ossl_wbio = BIO_new(BIO_s_mem());
    BIO_set_close(ssock->ossl_rbio, BIO_CLOSE);
    BIO_set_close(ssock->ossl_wbio, BIO_CLOSE);
    SSL_set_bio(ssock->ossl_ssl, ssock->ossl_rbio, ssock->ossl_wbio);

    /* Prepare read buffer */
    ssock->asock_rbuf = (void**)pj_pool_calloc(ssock->pool, 
					       ssock->param.async_cnt,
					       sizeof(void*));
    for (i = 0; i<ssock->param.async_cnt; ++i) {
	ssock->asock_rbuf[i] = (void*) pj_pool_alloc(
					    ssock->pool, 
					    ssock->param.read_buffer_size + 
					    sizeof(read_data_t*));
    }

    /* Start read */
    status = pj_activesock_start_read2(ssock->asock, ssock->pool, 
				       ssock->param.read_buffer_size,
				       ssock->asock_rbuf,
				       PJ_IOQUEUE_ALWAYS_ASYNC);
    if (status != PJ_SUCCESS)
	goto on_return;

    /* Prepare write/send state */
    pj_assert(ssock->write_state.max_len == 0);
    ssock->write_state.buf = (char*)
			     pj_pool_alloc(ssock->pool, 
					   ssock->param.send_buffer_size);
    ssock->write_state.max_len = ssock->param.send_buffer_size;
    ssock->write_state.start = ssock->write_state.buf;
    ssock->write_state.len = 0;

    /* Start SSL handshake */
    ssock->ssl_state = SSL_STATE_HANDSHAKING;
    SSL_set_connect_state(ssock->ossl_ssl);
    status = do_handshake(ssock);
    if (status != PJ_EPENDING)
	goto on_return;

    return PJ_TRUE;

on_return:
    return on_handshake_complete(ssock, status);
}



/*
 *******************************************************************
 * API
 *******************************************************************
 */

/* Load credentials from files. */
PJ_DEF(pj_status_t) pj_ssl_cert_load_from_files (pj_pool_t *pool,
						 const pj_str_t *CA_file,
						 const pj_str_t *cert_file,
						 const pj_str_t *privkey_file,
						 const pj_str_t *privkey_pass,
						 pj_ssl_cert_t **p_cert)
{
    pj_ssl_cert_t *cert;

    PJ_ASSERT_RETURN(pool && CA_file && cert_file && privkey_file, PJ_EINVAL);

    cert = PJ_POOL_ZALLOC_T(pool, pj_ssl_cert_t);
    pj_strdup_with_null(pool, &cert->CA_file, CA_file);
    pj_strdup_with_null(pool, &cert->cert_file, cert_file);
    pj_strdup_with_null(pool, &cert->privkey_file, privkey_file);
    pj_strdup_with_null(pool, &cert->privkey_pass, privkey_pass);

    *p_cert = cert;

    return PJ_SUCCESS;
}


/* Set SSL socket credentials. */
PJ_DECL(pj_status_t) pj_ssl_sock_set_certificate(
					    pj_ssl_sock_t *ssock,
					    pj_pool_t *pool,
					    const pj_ssl_cert_t *cert)
{
    PJ_ASSERT_RETURN(ssock && pool && cert, PJ_EINVAL);

    ssock->cert = PJ_POOL_ZALLOC_T(pool, pj_ssl_cert_t);
    pj_strdup_with_null(pool, &ssock->cert->CA_file, &cert->CA_file);
    pj_strdup_with_null(pool, &ssock->cert->cert_file, &cert->cert_file);
    pj_strdup_with_null(pool, &ssock->cert->privkey_file, &cert->privkey_file);
    pj_strdup_with_null(pool, &ssock->cert->privkey_pass, &cert->privkey_pass);

    return PJ_SUCCESS;
}


/* Get available ciphers. */
PJ_DEF(pj_status_t) pj_ssl_cipher_get_availables(pj_ssl_cipher ciphers[],
					         unsigned *cipher_num)
{
    unsigned i;

    PJ_ASSERT_RETURN(ciphers && cipher_num, PJ_EINVAL);

    if (openssl_cipher_num == 0) {
	init_openssl();
	shutdown_openssl();
    }

    if (openssl_cipher_num == 0)
	return PJ_ENOTFOUND;

    *cipher_num = PJ_MIN(*cipher_num, openssl_cipher_num);

    for (i = 0; i < *cipher_num; ++i)
	ciphers[i] = openssl_ciphers[i];

    return PJ_SUCCESS;
}


/*
 * Create SSL socket instance. 
 */
PJ_DEF(pj_status_t) pj_ssl_sock_create (pj_pool_t *pool,
					const pj_ssl_sock_param *param,
					pj_ssl_sock_t **p_ssock)
{
    pj_ssl_sock_t *ssock;
    pj_status_t status;

    PJ_ASSERT_RETURN(pool && param && p_ssock, PJ_EINVAL);
    PJ_ASSERT_RETURN(param->sock_type == pj_SOCK_STREAM(), PJ_ENOTSUP);

    pool = pj_pool_create(pool->factory, "ssl%p", 512, 512, NULL);

    /* Create secure socket */
    ssock = PJ_POOL_ZALLOC_T(pool, pj_ssl_sock_t);
    ssock->pool = pool;
    ssock->sock = PJ_INVALID_SOCKET;
    ssock->ssl_state = SSL_STATE_NULL;
    pj_list_init(&ssock->write_pending);
    pj_list_init(&ssock->write_pending_empty);

    /* Create secure socket mutex */
    status = pj_lock_create_recursive_mutex(pool, pool->obj_name,
					    &ssock->write_mutex);
    if (status != PJ_SUCCESS)
	return status;

    /* Init secure socket param */
    ssock->param = *param;
    ssock->param.read_buffer_size = ((ssock->param.read_buffer_size+7)>>3)<<3;
    if (param->ciphers_num > 0) {
	unsigned i;
	ssock->param.ciphers = (pj_ssl_cipher*)
			       pj_pool_calloc(pool, param->ciphers_num, 
					      sizeof(pj_ssl_cipher));
	for (i = 0; i < param->ciphers_num; ++i)
	    ssock->param.ciphers[i] = param->ciphers[i];
    }
    pj_strdup_with_null(pool, &ssock->param.servername, 
			&param->servername);

    /* Finally */
    *p_ssock = ssock;

    return PJ_SUCCESS;
}


/*
 * Close the secure socket. This will unregister the socket from the
 * ioqueue and ultimately close the socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_close(pj_ssl_sock_t *ssock)
{
    pj_pool_t *pool;

    PJ_ASSERT_RETURN(ssock, PJ_EINVAL);

    reset_ssl_sock_state(ssock);
    pj_lock_destroy(ssock->write_mutex);
    
    pool = ssock->pool;
    ssock->pool = NULL;
    if (pool)
	pj_pool_release(pool);

    return PJ_SUCCESS;
}


/*
 * Associate arbitrary data with the secure socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_set_user_data(pj_ssl_sock_t *ssock,
					      void *user_data)
{
    PJ_ASSERT_RETURN(ssock, PJ_EINVAL);

    ssock->param.user_data = user_data;
    return PJ_SUCCESS;
}


/*
 * Retrieve the user data previously associated with this secure
 * socket.
 */
PJ_DEF(void*) pj_ssl_sock_get_user_data(pj_ssl_sock_t *ssock)
{
    PJ_ASSERT_RETURN(ssock, NULL);

    return ssock->param.user_data;
}


/*
 * Retrieve the local address and port used by specified SSL socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_get_info (pj_ssl_sock_t *ssock,
					  pj_ssl_sock_info *info)
{
    pj_bzero(info, sizeof(*info));

    /* Established flag */
    info->established = (ssock->ssl_state == SSL_STATE_ESTABLISHED);

    /* Protocol */
    info->proto = ssock->param.proto;

    /* Local address */
    pj_sockaddr_cp(&info->local_addr, &ssock->local_addr);
    
    if (info->established) {
	/* Current cipher */
	const SSL_CIPHER *cipher;

	cipher = SSL_get_current_cipher(ssock->ossl_ssl);
	info->cipher = (cipher->id & 0x00FFFFFF);

	/* Remote address */
	pj_sockaddr_cp(&info->remote_addr, &ssock->rem_addr);
    }

    return PJ_SUCCESS;
}


/*
 * Starts read operation on this secure socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_read (pj_ssl_sock_t *ssock,
					    pj_pool_t *pool,
					    unsigned buff_size,
					    pj_uint32_t flags)
{
    void **readbuf;
    unsigned i;

    PJ_ASSERT_RETURN(ssock && pool && buff_size, PJ_EINVAL);
    PJ_ASSERT_RETURN(ssock->ssl_state==SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    readbuf = (void**) pj_pool_calloc(pool, ssock->param.async_cnt, 
				      sizeof(void*));

    for (i=0; i<ssock->param.async_cnt; ++i) {
	readbuf[i] = pj_pool_alloc(pool, buff_size);
    }

    return pj_ssl_sock_start_read2(ssock, pool, buff_size, 
				   readbuf, flags);
}


/*
 * Same as #pj_ssl_sock_start_read(), except that the application
 * supplies the buffers for the read operation so that the acive socket
 * does not have to allocate the buffers.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_read2 (pj_ssl_sock_t *ssock,
					     pj_pool_t *pool,
					     unsigned buff_size,
					     void *readbuf[],
					     pj_uint32_t flags)
{
    unsigned i;

    PJ_ASSERT_RETURN(ssock && pool && buff_size && readbuf, PJ_EINVAL);
    PJ_ASSERT_RETURN(ssock->ssl_state==SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    /* Create SSL socket read buffer */
    ssock->ssock_rbuf = (read_data_t*)pj_pool_calloc(pool, 
					       ssock->param.async_cnt,
					       sizeof(read_data_t));

    /* Store SSL socket read buffer pointer in the activesock read buffer */
    for (i=0; i<ssock->param.async_cnt; ++i) {
	read_data_t **p_ssock_rbuf = 
			OFFSET_OF_READ_DATA_PTR(ssock, ssock->asock_rbuf[i]);

	ssock->ssock_rbuf[i].data = readbuf[i];
	ssock->ssock_rbuf[i].len = 0;

	*p_ssock_rbuf = &ssock->ssock_rbuf[i];
    }

    ssock->read_size = buff_size;
    ssock->read_started = PJ_TRUE;
    ssock->read_flags = flags;

    return PJ_SUCCESS;
}


/*
 * Same as pj_ssl_sock_start_read(), except that this function is used
 * only for datagram sockets, and it will trigger \a on_data_recvfrom()
 * callback instead.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_recvfrom (pj_ssl_sock_t *ssock,
						pj_pool_t *pool,
						unsigned buff_size,
						pj_uint32_t flags)
{
    PJ_UNUSED_ARG(ssock);
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(buff_size);
    PJ_UNUSED_ARG(flags);

    return PJ_ENOTSUP;
}


/*
 * Same as #pj_ssl_sock_start_recvfrom() except that the recvfrom() 
 * operation takes the buffer from the argument rather than creating
 * new ones.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_recvfrom2 (pj_ssl_sock_t *ssock,
						 pj_pool_t *pool,
						 unsigned buff_size,
						 void *readbuf[],
						 pj_uint32_t flags)
{
    PJ_UNUSED_ARG(ssock);
    PJ_UNUSED_ARG(pool);
    PJ_UNUSED_ARG(buff_size);
    PJ_UNUSED_ARG(readbuf);
    PJ_UNUSED_ARG(flags);

    return PJ_ENOTSUP;
}

/* Write plain data to SSL and flush write BIO. Note that accessing
 * write BIO must be serialized, so a call to this function must be
 * protected by write mutex of SSL socket.
 */
static pj_status_t ssl_write(pj_ssl_sock_t *ssock, 
			     pj_ioqueue_op_key_t *send_key,
			     const void *data,
			     pj_ssize_t size,
			     unsigned flags)
{
    pj_status_t status;
    int nwritten;

    /* Write the plain data to SSL, after SSL encrypts it, write BIO will
     * contain the secured data to be sent via socket. Note that re-
     * negotitation may be on progress, so sending data should be delayed
     * until re-negotiation is completed.
     */
    nwritten = SSL_write(ssock->ossl_ssl, data, size);
    
    if (nwritten == size) {
	/* All data written, flush write BIO to network socket */
	status = flush_write_bio(ssock, send_key, size, flags);
    } else if (nwritten <= 0) {
	/* SSL failed to process the data, it may just that re-negotiation
	 * is on progress.
	 */
	int err;
	err = SSL_get_error(ssock->ossl_ssl, nwritten);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_NONE) {
	    /* Re-negotiation is on progress, flush re-negotiation data */
	    status = flush_write_bio(ssock, &ssock->handshake_op_key, 0, 0);
	    if (status == PJ_SUCCESS || status == PJ_EPENDING)
		status = PJ_EBUSY;
	} else {
	    /* Some problem occured */
	    ssl_report_error(THIS_FILE, 4, PJ_SUCCESS, "SSL_write()");
	    status = PJ_ECANCELLED;
	}
    } else {
	/* nwritten < *size, shouldn't happen, unless write BIO cannot hold 
	 * the whole secured data, perhaps because of insufficient memory.
	 */
	status = PJ_ENOMEM;
    }

    return status;
}

/* Flush delayed data sending in the write pending list. Note that accessing
 * write pending list must be serialized, so a call to this function must be
 * protected by write mutex of SSL socket.
 */
static pj_status_t flush_delayed_send(pj_ssl_sock_t *ssock)
{
    while (!pj_list_empty(&ssock->write_pending)) {
        write_pending_t *wp;
	pj_status_t status;

	wp = ssock->write_pending.next;

	status = ssl_write(ssock, &wp->data.key, wp->data.data.ptr, 
			   wp->data.plain_data_len, wp->data.flags);
	if (status != PJ_SUCCESS)
	    return status;

	pj_list_erase(wp);
	pj_list_push_back(&ssock->write_pending_empty, wp);
    }

    return PJ_SUCCESS;
}

/* Sending is delayed, push back the sending data into pending list. Note that
 * accessing write pending list must be serialized, so a call to this function
 * must be protected by write mutex of SSL socket.
 */
static pj_status_t delay_send (pj_ssl_sock_t *ssock,
			       pj_ioqueue_op_key_t *send_key,
			       const void *data,
			       pj_ssize_t size,
			       unsigned flags)
{
    write_pending_t *wp;

    /* Init write pending instance */
    if (!pj_list_empty(&ssock->write_pending_empty)) {
	wp = ssock->write_pending_empty.next;
	pj_list_erase(wp);
    } else {
	wp = PJ_POOL_ZALLOC_T(ssock->pool, write_pending_t);
    }

    wp->data.app_key = send_key;
    wp->data.plain_data_len = size;
    wp->data.data.ptr = data;
    wp->data.flags = flags;

    pj_list_push_back(&ssock->write_pending, wp);

    /* Must return PJ_EPENDING */
    return PJ_EPENDING;
}

/**
 * Send data using the socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_send (pj_ssl_sock_t *ssock,
				      pj_ioqueue_op_key_t *send_key,
				      const void *data,
				      pj_ssize_t *size,
				      unsigned flags)
{
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock && data && size && (*size>0), PJ_EINVAL);
    PJ_ASSERT_RETURN(ssock->ssl_state==SSL_STATE_ESTABLISHED, PJ_EINVALIDOP);

    pj_lock_acquire(ssock->write_mutex);

    /* Flush delayed send first. Sending data might be delayed when 
     * re-negotiation is on-progress.
     */
    status = flush_delayed_send(ssock);
    if (status == PJ_EBUSY) {
	/* Re-negotiation is on progress, delay sending */
	status = delay_send(ssock, send_key, data, *size, flags);
	goto on_return;
    } else if (status != PJ_SUCCESS) {
	goto on_return;
    }

    /* Write data to SSL */
    status = ssl_write(ssock, send_key, data, *size, flags);
    if (status == PJ_EBUSY) {
	/* Re-negotiation is on progress, delay sending */
	status = delay_send(ssock, send_key, data, *size, flags);
    }

on_return:
    pj_lock_release(ssock->write_mutex);
    return status;
}


/**
 * Send datagram using the socket.
 */
PJ_DEF(pj_status_t) pj_ssl_sock_sendto (pj_ssl_sock_t *ssock,
					pj_ioqueue_op_key_t *send_key,
					const void *data,
					pj_ssize_t *size,
					unsigned flags,
					const pj_sockaddr_t *addr,
					int addr_len)
{
    PJ_UNUSED_ARG(ssock);
    PJ_UNUSED_ARG(send_key);
    PJ_UNUSED_ARG(data);
    PJ_UNUSED_ARG(size);
    PJ_UNUSED_ARG(flags);
    PJ_UNUSED_ARG(addr);
    PJ_UNUSED_ARG(addr_len);

    return PJ_ENOTSUP;
}


/**
 * Starts asynchronous socket accept() operations on this secure socket. 
 */
PJ_DEF(pj_status_t) pj_ssl_sock_start_accept (pj_ssl_sock_t *ssock,
					      pj_pool_t *pool,
					      const pj_sockaddr_t *localaddr,
					      int addr_len)
{
    pj_activesock_cb asock_cb;
    pj_activesock_cfg asock_cfg;
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock && pool && localaddr && addr_len, PJ_EINVAL);

    /* Create socket */
    status = pj_sock_socket(ssock->param.sock_af, ssock->param.sock_type, 0, 
			    &ssock->sock);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Bind socket */
    status = pj_sock_bind(ssock->sock, localaddr, addr_len);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Start listening to the address */
    status = pj_sock_listen(ssock->sock, PJ_SOMAXCONN);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = ssock->param.async_cnt;
    asock_cfg.concurrency = ssock->param.concurrency;
    asock_cfg.whole_data = PJ_TRUE;

    pj_bzero(&asock_cb, sizeof(asock_cb));
    asock_cb.on_accept_complete = asock_on_accept_complete;

    status = pj_activesock_create(pool,
				  ssock->sock, 
				  ssock->param.sock_type,
				  &asock_cfg,
				  ssock->param.ioqueue, 
				  &asock_cb,
				  ssock,
				  &ssock->asock);

    if (status != PJ_SUCCESS)
	goto on_error;

    /* Start accepting */
    status = pj_activesock_start_accept(ssock->asock, pool);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Update local address */
    ssock->addr_len = addr_len;
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr, 
				 &ssock->addr_len);
    if (status != PJ_SUCCESS)
	pj_sockaddr_cp(&ssock->local_addr, localaddr);

    ssock->is_server = PJ_TRUE;

    return PJ_SUCCESS;

on_error:
    reset_ssl_sock_state(ssock);
    return status;
}


/**
 * Starts asynchronous socket connect() operation.
 */
PJ_DECL(pj_status_t) pj_ssl_sock_start_connect(pj_ssl_sock_t *ssock,
					       pj_pool_t *pool,
					       const pj_sockaddr_t *localaddr,
					       const pj_sockaddr_t *remaddr,
					       int addr_len)
{
    pj_activesock_cb asock_cb;
    pj_activesock_cfg asock_cfg;
    pj_status_t status;

    PJ_ASSERT_RETURN(ssock && pool && localaddr && remaddr && addr_len,
		     PJ_EINVAL);

    /* Create socket */
    status = pj_sock_socket(ssock->param.sock_af, ssock->param.sock_type, 0, 
			    &ssock->sock);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Bind socket */
    status = pj_sock_bind(ssock->sock, localaddr, addr_len);
    if (status != PJ_SUCCESS)
	goto on_error;

    /* Create active socket */
    pj_activesock_cfg_default(&asock_cfg);
    asock_cfg.async_cnt = ssock->param.async_cnt;
    asock_cfg.concurrency = ssock->param.concurrency;
    asock_cfg.whole_data = PJ_TRUE;

    pj_bzero(&asock_cb, sizeof(asock_cb));
    asock_cb.on_connect_complete = asock_on_connect_complete;
    asock_cb.on_data_read = asock_on_data_read;
    asock_cb.on_data_sent = asock_on_data_sent;

    status = pj_activesock_create(pool,
				  ssock->sock, 
				  ssock->param.sock_type,
				  &asock_cfg,
				  ssock->param.ioqueue, 
				  &asock_cb,
				  ssock,
				  &ssock->asock);

    if (status != PJ_SUCCESS)
	goto on_error;

    status = pj_activesock_start_connect(ssock->asock, pool, remaddr,
					 addr_len);

    if (status == PJ_SUCCESS)
	asock_on_connect_complete(ssock->asock, PJ_SUCCESS);
    else if (status != PJ_EPENDING)
	goto on_error;

    /* Update local address */
    ssock->addr_len = addr_len;
    status = pj_sock_getsockname(ssock->sock, &ssock->local_addr,
				 &ssock->addr_len);
    if (status != PJ_SUCCESS)
	pj_sockaddr_cp(&ssock->local_addr, localaddr);

    /* Set remote address */
    pj_sockaddr_cp(&ssock->rem_addr, remaddr);

    /* Update SSL state */
    ssock->is_server = PJ_FALSE;

    return PJ_EPENDING;

on_error:
    reset_ssl_sock_state(ssock);
    return status;
}


//#endif  /* PJ_HAS_SSL_SOCK */
