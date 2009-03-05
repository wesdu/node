#include "node_tcp.h"
#include "node.h"

#include <oi_socket.h>
#include <oi_buf.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

using namespace v8;

static Persistent<String> readyState_str; 

static Persistent<Integer> readyState_CONNECTING; 
static Persistent<Integer> readyState_OPEN; 
static Persistent<Integer> readyState_CLOSED; 

enum readyState { READY_STATE_CONNECTING = 0
                , READY_STATE_OPEN       = 1
                , READY_STATE_CLOSED     = 2
                } ;

class TCPClient {
public:
  TCPClient(Handle<Object> obj);
  ~TCPClient();

  int Connect(char *host, char *port);
  void Write (Handle<Value> arg);
  void Disconnect();

  void OnOpen();

private:
  oi_socket socket;
  struct addrinfo *address;
  Persistent<Object> js_client;
};


static void on_connect
  ( oi_socket *socket
  )
{
  TCPClient *client = static_cast<TCPClient*> (socket->data);
  client->OnOpen();
}

static struct addrinfo tcp_hints = 
/* ai_flags      */ { AI_PASSIVE
/* ai_family     */ , AF_UNSPEC
/* ai_socktype   */ , SOCK_STREAM
/* ai_protocol   */ , 0
/* ai_addrlen    */ , 0
/* ai_addr       */ , 0
/* ai_canonname  */ , 0
/* ai_next       */ , 0
                    };

static Handle<Value> newTCPClient
  ( const Arguments& args
  ) 
{
  if (args.Length() < 1)
    return Undefined();

  HandleScope scope;

  String::AsciiValue host(args[0]);
  String::AsciiValue port(args[1]);

  TCPClient *client = new TCPClient(args.This());
  if(client == NULL)
    return Undefined(); // XXX raise error?

  int r = client->Connect(*host, *port);
  if (r != 0)
    return Undefined(); // XXX raise error?


  return args.This();
}

static TCPClient*
UnwrapClient (Handle<Object> obj) 
{
  HandleScope scope;
  Handle<External> field = Handle<External>::Cast(obj->GetInternalField(0));
  TCPClient* client = static_cast<TCPClient*>(field->Value());
  return client;
}

static Handle<Value>
WriteCallback (const Arguments& args) 
{
  HandleScope scope;
  TCPClient *client = UnwrapClient(args.Holder());
  client->Write(args[0]);
  return Undefined();
}

static Handle<Value>
DisconnectCallback (const Arguments& args) 
{
  HandleScope scope;
  TCPClient *client = UnwrapClient(args.Holder());
  client->Disconnect();
  return Undefined();
}

static void
client_destroy (Persistent<Value> _, void *data)
{
  TCPClient *client = static_cast<TCPClient *> (data);
  delete client;
}

TCPClient::TCPClient(Handle<Object> _js_client)
{
  oi_socket_init(&socket, 30.0); // TODO adjustable timeout
  socket.on_connect = on_connect;
  socket.on_read    = NULL;
  socket.on_drain   = NULL;
  socket.on_error   = NULL;
  socket.on_close   = NULL;
  socket.on_timeout = NULL;
  socket.data = this;

  HandleScope scope;
  js_client = Persistent<Object>::New(_js_client);
  js_client->SetInternalField (0, External::New(this));
  js_client.MakeWeak (this, client_destroy);
}

TCPClient::~TCPClient ()
{
  Disconnect();
  oi_socket_detach (&socket);
  js_client.Dispose();
  js_client.Clear(); // necessary? 
}

int
TCPClient::Connect(char *host, char *port)
{
  int r;

  HandleScope scope;

  js_client->Set(readyState_str, readyState_CONNECTING);

  /* FIXME Blocking DNS resolution. Use oi_async. */
  printf("resolving host: %s, port: %s\n", host, port);
  r = getaddrinfo (host, port, &tcp_hints, &address);
  if(r != 0)  {
    perror("getaddrinfo");
    return r;
  }

  r = oi_socket_connect (&socket, address);
  if(r != 0)  {
    perror("oi_socket_connect");
    return r;
  }
  oi_socket_attach (&socket, node_loop());

  freeaddrinfo(address);
  address = NULL;
}

void TCPClient::Write (Handle<Value> arg)
{
  HandleScope scope;

  if(arg == Null()) {

    oi_socket_write_eof(&socket);

  } else {
    Local<String> s = arg->ToString();

    oi_buf *buf = oi_buf_new2(s->Length());
    s->WriteAscii(buf->base, 0, s->Length());

    oi_socket_write(&socket, buf);
  }
}
 
void
TCPClient::Disconnect()
{
  oi_socket_close(&socket);
}

void TCPClient::OnOpen()
{
  HandleScope scope;

  js_client->Set(readyState_str, readyState_OPEN);

  Handle<Value> onopen_value = js_client->Get( String::NewSymbol("onopen") );
  if (!onopen_value->IsFunction())
    return; 
  Handle<Function> onopen = Handle<Function>::Cast(onopen_value);

  TryCatch try_catch;

  Handle<Value> r = onopen->Call(js_client, 0, NULL);

  if(try_catch.HasCaught())
    node_fatal_exception(try_catch);
}

void
Init_tcp (Handle<Object> target)
{
  HandleScope scope;
  readyState_str = Persistent<String>::New(String::NewSymbol("readyState"));

  Local<FunctionTemplate> client_t = FunctionTemplate::New(newTCPClient);

  client_t->InstanceTemplate()->SetInternalFieldCount(1);

  /* readyState constants */

  readyState_CONNECTING = Persistent<Integer>::New(Integer::New(READY_STATE_CONNECTING));
  client_t->InstanceTemplate()->Set ( String::NewSymbol("CONNECTING")
                                    , readyState_CONNECTING
                                    );

  readyState_OPEN = Persistent<Integer>::New(Integer::New(READY_STATE_OPEN));
  client_t->InstanceTemplate()->Set ( String::NewSymbol("OPEN")
                                    , readyState_OPEN
                                    );

  readyState_CLOSED = Persistent<Integer>::New(Integer::New(READY_STATE_CLOSED));
  client_t->InstanceTemplate()->Set ( String::NewSymbol("CLOSED")
                                    , readyState_CLOSED
                                    );

  /* write callback */

  Local<FunctionTemplate> write_t = FunctionTemplate::New(WriteCallback);

  client_t->InstanceTemplate()->Set ( String::NewSymbol("write")
                                    , write_t->GetFunction()
                                    );

  /* disconnect callback */

  Local<FunctionTemplate> disconnect_t = FunctionTemplate::New(DisconnectCallback);

  client_t->InstanceTemplate()->Set ( String::NewSymbol("disconnect")
                                    , disconnect_t->GetFunction()
                                    );

  target->Set(String::NewSymbol("TCPClient"), client_t->GetFunction());
}
