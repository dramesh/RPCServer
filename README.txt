Files
=====

Source
------
msg.x - RPC IDX
proxy.c - proxy server implementation
rprintmsg.c - client implementation


Input URL list
--------------
links

Input files
-----------
loc_1000.txt
biased_1000.txt
freq_1000.txt
rand_1000.txt
loop_1000.txt

How to compile & run
====================
gcc proxy.c msg_svc.c -o proxy_server -lpthread -lresolv -lnsl -lcurl -g
gcc rprintmsg.c msg_clnt.c -o proxy_client -g
./proxy_server &
./proxy_client <hostname> <inputfile>

In proxy.c, update cr_policy to LRU/LFU/Rand accordingly.