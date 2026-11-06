import sys, threading, hashlib, time, re, os
from wsgidav.wsgidav_app import WsgiDAVApp
from cheroot import wsgi
from cheroot.ssl.builtin import BuiltinSSLAdapter
root, base, certdir = sys.argv[1], int(sys.argv[2]), sys.argv[3]
def app(accept_basic=True, accept_digest=False):
    return WsgiDAVApp({'provider_mapping':{'/':root},'verbose':0,'logging':{'enable':False},
      'http_authenticator':{'domain_controller':None,'accept_basic':accept_basic,'accept_digest':accept_digest,'default_to_digest':accept_digest},
      'simple_dc':{'user_mapping':{'*':{'test':{'password':'secret'}}}}, 'lock_storage':True,'dir_browser':{'enable':False},'mutable_live_props':['{DAV:}getlastmodified']})
def redirects(inner, other):
    def w(env, sr):
        p = env.get('PATH_INFO','')
        if p.startswith('/redirect/'):
            sr('301 Moved',[('Location',p[len('/redirect'):]),('Content-Length','0')]); return [b'']
        if p.startswith('/cross-redirect/'):
            sr('302 Found',[('Location','http://localhost:%d%s'%(other,p[len('/cross-redirect'):])),('Content-Length','0')]); return [b'']
        return inner(env, sr)
    return w
def serve(a, port, tls=False):
    s = wsgi.Server(('127.0.0.1',port),a,numthreads=8)
    if tls: s.ssl_adapter = BuiltinSSLAdapter(certdir+'/server.crt',certdir+'/server.key')
    threading.Thread(target=s.start,daemon=True).start()
serve(redirects(app(),base),base)
serve(redirects(app(),base),base+1,True)
serve(redirects(app(False,True),base+2),base+2)
threading.Event().wait()
