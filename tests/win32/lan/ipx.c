/* Two-guest raw Winsock IPX ping/echo. SPDX-License-Identifier: MIT */
#include "winsock2.h"
#include "wsipx.h"
#include "wsnwlink.h"
#include "common.h"
int WINAPI entry(void)
{
    WSADATA data;SOCKET sock;SOCKADDR_IPX local={0},remote={0};BOOL yes=TRUE;
    char packet[16]="CIUKI-IPX-LAN2",received[32];int host=option(" host"),size;
    require(!WSAStartup(MAKEWORD(2,0),&data),"FAIL WSAStartup\r\n");
    sock=socket(AF_IPX,SOCK_DGRAM,NSPROTO_IPX+4);require(sock!=INVALID_SOCKET,"FAIL IPX socket\r\n");
    local.sa_family=AF_IPX;local.sa_socket=htons(0x6c32);
    require(!bind(sock,(SOCKADDR *)&local,sizeof(local)),"FAIL IPX bind\r\n");
    size=sizeof(local);require(!getsockname(sock,(SOCKADDR *)&local,&size),"FAIL local IPX address\r\n");
    if(!host){
        remote=local;memset(remote.sa_nodenum,255,6);
        require(!setsockopt(sock,SOL_SOCKET,SO_BROADCAST,(const char *)&yes,sizeof(yes)),"FAIL broadcast option\r\n");
        require(sendto(sock,packet,sizeof(packet),0,(SOCKADDR *)&remote,sizeof(remote))==sizeof(packet),"FAIL broadcast discovery\r\n");
    }
    log_line(host ? "LAN2 IPX host ready\r\n" : "LAN2 IPX joining\r\n");
    DWORD start=GetTickCount();int done=0;
    while(GetTickCount()-start<60000&&!done){
        fd_set read={0};struct timeval wait={1,0};FD_SET(sock,&read);
        int ready=select(0,&read,NULL,NULL,&wait);require(ready>=0,"FAIL IPX select\r\n");if(!ready)continue;
        size=sizeof(remote);int n=recvfrom(sock,received,sizeof(received),0,(SOCKADDR *)&remote,&size);
        if(n!=sizeof(packet)||size!=sizeof(remote)||memcmp(received,packet,sizeof(packet))||!memcmp(remote.sa_nodenum,local.sa_nodenum,6))continue;
        if(host)require(sendto(sock,packet,sizeof(packet),0,(SOCKADDR *)&remote,size)==sizeof(packet),"FAIL directed echo\r\n");
        done=1;
    }
    closesocket(sock);WSACleanup();require(done,"FAIL IPX timeout\r\n");log_line("PASS LAN2 Win32 AF_IPX broadcast/directed echo\r\n");ExitProcess(0);return 0;
}
