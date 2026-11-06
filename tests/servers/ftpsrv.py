import sys, threading, asyncio
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
from pyftpdlib.servers import ThreadedFTPServer
root, base, cert, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
def auth():
    a = DummyAuthorizer(); a.add_user('test','secret',root,perm='elradfmwMT'); a.add_anonymous(root); return a
class Plain(FTPHandler):
    authorizer = auth(); banner='pyftpdlib e2e'; permit_foreign_addresses=True
class NoMlsd(FTPHandler):
    authorizer = auth(); banner='pyftpdlib e2e list'
    proto_cmds = {k:v for k,v in FTPHandler.proto_cmds.items() if k not in ('MLSD','MLST')}
class Explicit(TLS_FTPHandler):
    authorizer = auth(); certfile=cert; tls_control_required=False; tls_data_required=False
class Implicit(TLS_FTPHandler):
    authorizer = auth(); certfile=cert
    def handle(self):
        self.secure_connection(self.get_ssl_context())
    def handle_ssl_established(self):
        FTPHandler.handle(self)
h,p = {'plain':(Plain,base),'explicit':(Explicit,base+1),'implicit':(Implicit,base+2),'list':(NoMlsd,base+3)}[mode]
ThreadedFTPServer(('127.0.0.1',p),h).serve_forever()
