#include "SSL.h"

#define LLOG(x) // DLOG(x)

NAMESPACE_UPP

struct TcpSocket::SSLImp : TcpSocket::SSL {
	virtual bool  Start();
	virtual bool  Wait(dword flags);
	virtual int   Send(const void *buffer, int maxlen);
	virtual int   Recv(void *buffer, int maxlen);
	virtual void  Close();
	virtual dword Handshake();

	TcpSocket&     socket;
	SslContext     context;
	::SSL         *ssl;
	SslCertificate cert;

	int            GetErrorCode(int res);
	String         GetErrorText(int code);
	void           SetSSLError(const char *context);
	void           SetSSLResError(const char *context, int res);
	bool           IsAgain(int res) const;
	
	SSLImp(TcpSocket& socket) : socket(socket) { ssl = NULL; }
	~SSLImp();
};

TcpSocket::SSL *TcpSocket::CreateSSLImp(TcpSocket& socket)
{
	return new TcpSocket::SSLImp(socket);
}

void InitCreateSSL()
{
	TcpSocket::CreateSSL = TcpSocket::CreateSSLImp;
}

INITBLOCK {
	InitCreateSSL();
}

TcpSocket::SSLImp::~SSLImp()
{
	if(ssl)
		SSL_free(ssl);
}

void TcpSocket::SSLImp::SetSSLError(const char *context)
{
	int code;
	String text = SslGetLastError(code);
	socket.SetSockError(context, code, text);
}

const char *TcpSocketErrorDesc(int code);

int TcpSocket::SSLImp::GetErrorCode(int res)
{
	return SSL_get_error(ssl, res);
}

String TcpSocket::SSLImp::GetErrorText(int code)
{
	String out;
	switch(code) {
#define SSLERR(c) case c: out = #c; break;
		SSLERR(SSL_ERROR_NONE)
		SSLERR(SSL_ERROR_SSL)
		SSLERR(SSL_ERROR_WANT_READ)
		SSLERR(SSL_ERROR_WANT_WRITE)
		SSLERR(SSL_ERROR_WANT_X509_LOOKUP)
		SSLERR(SSL_ERROR_SYSCALL)
		SSLERR(SSL_ERROR_ZERO_RETURN)
		SSLERR(SSL_ERROR_WANT_CONNECT)
#ifdef PLATFORM_WIN32
		SSLERR(SSL_ERROR_WANT_ACCEPT)
#endif
		default: out = "unknown code"; break;
	}
	return out;
}

void TcpSocket::SSLImp::SetSSLResError(const char *context, int res)
{
	int code = GetErrorCode(res);
	if(code == SSL_ERROR_SYSCALL) {
		socket.SetSockError(context);
		return;
	}
	socket.SetSockError(context, code, GetErrorText(code));
}

bool TcpSocket::SSLImp::IsAgain(int res) const
{
	res = SSL_get_error(ssl, res);
	return res == SSL_ERROR_WANT_READ ||
	       res == SSL_ERROR_WANT_WRITE ||
	       res == SSL_ERROR_WANT_CONNECT ||
	       res == SSL_ERROR_WANT_ACCEPT;
}

bool TcpSocket::SSLImp::Start()
{
	LLOG("SSL Start");

	if(!context.Create(const_cast<SSL_METHOD *>(SSLv3_client_method()))) {
		SetSSLError("Start: SSL context.");
		return false;
	}
//	context.VerifyPeer();
	if(!(ssl = SSL_new(context))) {
		SetSSLError("Start: SSL_new");
		return false;
	}
	if(!SSL_set_fd(ssl, socket.GetSOCKET())) {
		SetSSLError("Start: SSL_set_fd");
		return false;
	}
	return Handshake();
}

dword TcpSocket::SSLImp::Handshake()
{
	int res;
	if(socket.mode == ACCEPT)
		res = SSL_accept(ssl);
	else
	if(socket.mode == CONNECT)
		res = SSL_connect(ssl);
	else
		return 0;
	if(res <= 0) {
		int code = GetErrorCode(res);
		if(code == SSL_ERROR_WANT_READ)
			return WAIT_READ;
		if(code == SSL_ERROR_WANT_WRITE)
			return WAIT_WRITE;
		SetSSLResError("SSL handshake", res);
		return 0;
	}
	socket.mode = SSL_CONNECTED;
	cert.Set(SSL_get_peer_certificate(ssl));
	SSLInfo& f = socket.sslinfo.Create();
	f.cipher = SSL_get_cipher(ssl);
	if(!cert.IsEmpty()) {
		f.cert_avail = true;
		f.cert_subject = cert.GetSubjectName();
		f.cert_issuer = cert.GetIssuerName();
		f.cert_serial = cert.GetSerialNumber();
		f.cert_notbefore = cert.GetNotBefore();
		f.cert_notafter = cert.GetNotAfter();
		f.cert_version = cert.GetVersion();
		f.cert_verified = SSL_get_verify_result(ssl) == X509_V_OK;
	}
	return 0;
}

bool TcpSocket::SSLImp::Wait(dword flags)
{
	LLOG("SSL Wait");
	if((flags & WAIT_READ) && SSL_pending(ssl) > 0)
		return true;
	return socket.RawWait(flags);
}

int TcpSocket::SSLImp::Send(const void *buffer, int maxlen)
{
	LLOG("SSL Send " << maxlen);
	int res = SSL_write(ssl, (const char *)buffer, maxlen);
	if(res > 0)
		return res;
	if(res == 0)
		socket.is_eof = true;
	else
	if(!IsAgain(res))
		SetSSLResError("SSL_write", res);
	return 0;
}

int TcpSocket::SSLImp::Recv(void *buffer, int maxlen)
{
	LLOG("SSL Recv " << maxlen);
	int res = SSL_read(ssl, (char *)buffer, maxlen);
	if(res > 0)
		return res;
	if(res == 0)
		socket.is_eof = true;
	else
	if(!IsAgain(res))
		SetSSLResError("SSL_read", res);
	return 0;
}

void TcpSocket::SSLImp::Close()
{
	LLOG("SSL Close");
	SSL_shutdown(ssl);
	socket.RawClose();
	SSL_free(ssl);
	ssl = NULL;
}

END_UPP_NAMESPACE
