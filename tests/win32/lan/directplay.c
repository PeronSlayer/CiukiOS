/* Actual IDirectPlay4 COM session, TCP/IP or IPX. SPDX-License-Identifier: MIT */
#define COBJMACROS
#define INITGUID
#include "objbase.h"
#include "dplay.h"
#include "dplobby.h"
#include "common.h"
static const GUID application={0x63270210,0x7b23,0x4e12,{0x81,0x10,0x43,0x69,0x75,0x6b,0x69,0x32}};
static DPSESSIONDESC2 found;
static BOOL CALLBACK sessions(const DPSESSIONDESC2 *session,DWORD *timeout,DWORD flags,void *context)
{(void)timeout;(void)context;if(flags&DPESC_TIMEDOUT)return FALSE;found=*session;return FALSE;}
int WINAPI entry(void)
{
    IDirectPlay *base=NULL;IDirectPlay4 *play=NULL;IDirectPlayLobbyA *lobby_base=NULL;IDirectPlayLobby3A *lobby=NULL;
    GUID provider=option(" ipx") ? DPSPGUID_IPX : DPSPGUID_TCPIP;
    BYTE address[512];DWORD bytes=sizeof(address);DPCOMPOUNDADDRESSELEMENT parts[2];char inet[]="";
    require(SUCCEEDED(DirectPlayCreate(NULL,&base,NULL)),"FAIL DirectPlayCreate\r\n");
    require(SUCCEEDED(IDirectPlay_QueryInterface(base,&IID_IDirectPlay4A,(void **)&play)),"FAIL IDirectPlay4A\r\n");IDirectPlay_Release(base);
    require(SUCCEEDED(DirectPlayLobbyCreateA(NULL,&lobby_base,NULL,NULL,0)),"FAIL lobby create\r\n");
    require(SUCCEEDED(IDirectPlayLobby_QueryInterface(lobby_base,&IID_IDirectPlayLobby3A,(void **)&lobby)),"FAIL Lobby3A\r\n");IDirectPlayLobby_Release(lobby_base);
    parts[0]=(DPCOMPOUNDADDRESSELEMENT){DPAID_ServiceProvider,sizeof(provider),&provider};
    parts[1]=(DPCOMPOUNDADDRESSELEMENT){DPAID_INet,sizeof(inet),inet};
    require(SUCCEEDED(IDirectPlayLobby_CreateCompoundAddress(lobby,parts,option(" ipx") ? 1 : 2,address,&bytes)),"FAIL compound address\r\n");IDirectPlayLobby_Release(lobby);
    require(SUCCEEDED(IDirectPlayX_InitializeConnection(play,address,0)),"FAIL connection initialization\r\n");
    DPSESSIONDESC2 session={0};session.dwSize=sizeof(session);session.guidApplication=application;session.dwMaxPlayers=5;
    session.lpszSessionNameA="CiukiOS LAN2 fixture";int host=option(" host");
    if(host)require(SUCCEEDED(IDirectPlayX_Open(play,&session,DPOPEN_CREATE)),"FAIL session create\r\n");
    else {
        DWORD start=GetTickCount();while(!found.dwSize&&GetTickCount()-start<60000){
            require(SUCCEEDED(IDirectPlayX_EnumSessions(play,&session,1000,sessions,NULL,DPENUMSESSIONS_AVAILABLE)),"FAIL session enumeration\r\n");Sleep(250);
        }
        require(found.dwSize!=0,"FAIL session timeout\r\n");session.guidInstance=found.guidInstance;
        require(SUCCEEDED(IDirectPlayX_Open(play,&session,DPOPEN_JOIN)),"FAIL session join\r\n");
    }
    DPID player;DPNAME name={0};name.dwSize=sizeof(name);name.lpszShortNameA=host ? "host" : "joining peer";
    require(SUCCEEDED(IDirectPlayX_CreatePlayer(play,&player,&name,NULL,NULL,0,0)),"FAIL player create\r\n");
    char chat[]="CiukiOS LAN2 chat",buffer[2048];DWORD start=GetTickCount(),last=0;int received=0;
    log_line(host ? "LAN2 DirectPlay host ready\r\n" : "LAN2 DirectPlay joined\r\n");
    while(GetTickCount()-start<60000&&!received){
        DWORD now=GetTickCount();if(now-last>=1000){
            require(SUCCEEDED(IDirectPlayX_Send(play,player,DPID_ALLPLAYERS,DPSEND_GUARANTEED,chat,sizeof(chat))),"FAIL guaranteed send\r\n");last=now;
        }
        DPID from,to;DWORD n=sizeof(buffer);HRESULT hr=IDirectPlayX_Receive(play,&from,&to,DPRECEIVE_ALL,buffer,&n);
        if(hr==DPERR_NOMESSAGES){Sleep(10);continue;}
        require(SUCCEEDED(hr),"FAIL receive\r\n");
        if(from!=DPID_SYSMSG&&from!=player&&n==sizeof(chat)&&!memcmp(buffer,chat,n)){
            require(SUCCEEDED(IDirectPlayX_Send(play,player,from,DPSEND_GUARANTEED,chat,sizeof(chat))),"FAIL guaranteed reply\r\n");received=1;
        }
    }
    IDirectPlayX_DestroyPlayer(play,player);IDirectPlayX_Close(play);IDirectPlayX_Release(play);
    require(received,"FAIL DirectPlay chat timeout\r\n");log_line("PASS LAN2 DirectPlay COM enumerate/open/player/guaranteed chat/close\r\n");ExitProcess(0);return 0;
}
