#!/bin/bash
# The certificates of tests/test_ftpex.cpp (NIXSCP_TEST_FTPX_CERTS): mkcerts.sh <dir>
#  ca.crt                the test CA "NixSCP WP07 CA", self-signed
#  server-chain.pem      server certificate for localhost + the CA: the server
#                        sends the CA too (the test expects the verification
#                        error "self-signed certificate in chain" at depth 2)
#  client-combined.pem   encrypted key (passphrase "certpass") + certificate
#  client.p12            the same, PKCS#12 (passphrase "certpass")
#  client-sep.key/.crt   key without passphrase, certificate next to it
#  client-ed.key         key without any certificate
#  client.csr            a request: neither key nor certificate
set -e
D=$1
mkdir -p "$D"
cd "$D"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 3650 \
  -subj "/O=NixSCP WP07 CA/CN=NixSCP WP07 CA" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj "/O=NixSCP WP07/CN=localhost" 2>/dev/null
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 3650 \
  -extfile <(printf 'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n') 2>/dev/null
cat server.crt ca.crt > server-chain.pem
openssl req -newkey rsa:2048 -nodes -keyout client-sep.key -out client.csr -subj "/CN=wp07 client" 2>/dev/null
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out client-sep.crt -days 3650 \
  -extfile <(printf 'extendedKeyUsage=clientAuth\n') 2>/dev/null
openssl pkcs8 -topk8 -in client-sep.key -passout pass:certpass -out client-enc.key
cat client-enc.key client-sep.crt > client-combined.pem
openssl pkcs12 -export -inkey client-sep.key -in client-sep.crt -passout pass:certpass -out client.p12
openssl genpkey -algorithm ed25519 -out client-ed.key
rm -f client-enc.key server.csr ca.srl
chmod 600 ./*.key client-combined.pem client.p12
