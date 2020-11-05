# Choosing a transport implementation

1.  Go into the sub directory for the TCP/IP stack you are using.
2.  Build the wrapper file located in the directory (i.e. `freertos_plus_tcp_secure_sockets_wrapper.c`).
3.  Select an additional file to be correct for the TLS stack you are using (`freertos_plus_tcp_mbedtls.c`), or the plaintext file if not using TLS (`freertos_plus_tcp_plaintext.c`)
