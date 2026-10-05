/* User-mode worker transport. The desktop owns TCP retransmission and only
 * acknowledges received ciphertext after storing it in this bounded queue. */
static u32 tls_utc(void)
{
    struct regs r;u16 year,month,day,hour,minute,second,y,m;u32 days=0;
    static const u8 mdays[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    mem_set(&r,0,sizeof r);r.ax=0x2A00;intr(0x21,&r);
    year=r.cx;month=r.dx>>8;day=r.dx&255;
    if(year<2020||year>2105||month<1||month>12||day<1||day>31)return 0;
    for(y=1970;y<year;++y)days+=365+(y%4==0&&(y%100!=0||y%400==0));
    for(m=1;m<month;++m)days+=mdays[m-1]+(m==2&&year%4==0&&(year%100!=0||year%400==0));
    days+=day-1;mem_set(&r,0,sizeof r);r.ax=0x2C00;intr(0x21,&r);
    hour=r.cx>>8;minute=r.cx&255;second=r.dx>>8;
    if(hour>23||minute>59||second>59)return 0;
    return days*86400UL+(u32)hour*3600UL+minute*60UL+second;
}
static int tls_start(void)
{
    u32 utc=tls_utc();
    if(!utc){str_copy(error_text,"HTTPS needs a valid system date and time.");return 0;}
    if(!webwork_submit(CWW_TLS_OPEN,host,str_len(host)+1,utc)){
        (void)webwork_close();
        str_copy(error_text,"The HTTPS worker could not be started.");return 0;
    }
    tls_busy=1;tls_started=1;tls_job=CWW_TLS_OPEN;return 1;
}
static void tls_ack(u32 ack)
{
    u32 count;
    if(!tls_tx_bytes||!seq_after(ack,tls_tx_seq))return;
    count=ack-tls_tx_seq;if(count>tls_tx_bytes)return;
    tls_tx_bytes-=(u16)count;tls_tx_seq+=count;
    if(tls_tx_bytes)mem_move(tls_tx,tls_tx+(u16)count,tls_tx_bytes);
    tries=0;last_tick=HOST.ticks;
}
static void tls_error(u32 code)
{
    char text[80];
    if(code==0x20001UL)fail("HTTPS needs the virtio-rng entropy device.");
    else if(code>=0x21000UL&&code<0x21100UL){
        str_copy(text,"HTTPS entropy device initialization failed (stage ");
        fmt_u32(text+str_len(text),code-0x21000UL);str_cat(text,").");fail(text);
    }
    else if(code==0x20002UL)fail("The HTTPS certificate authority store is unavailable.");
    else{str_copy(text,"HTTPS certificate or handshake failed (code ");fmt_u32(text+str_len(text),code);str_cat(text,").");fail(text);}
}
static void tls_poll(void)
{
    u16 n;int result;
    if(tls_busy){
        result=webwork_poll(&tls_reply,0,0);
        if(!result)return;tls_busy=0;
        if(result<0){fail("The HTTPS worker stopped or timed out.");return;}
        if(tls_reply.error){tls_error(tls_reply.error);return;}
        if(tls_job==CWW_TLS_STEP){
            if(tls_reply.consumed>tls_rx_sent||tls_reply.consumed>tls_rx_bytes){fail("Invalid HTTPS worker response.");return;}
            tls_rx_bytes-=(u16)tls_reply.consumed;
            mem_move(tls_rx,tls_rx+(u16)tls_reply.consumed,tls_rx_bytes);
            if(state==S_HTTP)(void)tcp_send(0x10,0,0,snd_nxt,rcv_nxt);
        }else if(tls_job==CWW_TLS_WRITE){
            if(tls_reply.consumed!=request_len){fail("The HTTPS request could not be queued.");return;}
            request_sent=1;
        }else if(tls_job==CWW_TLS_CLOSE){
            tls_close_complete=1;
        }
        if(tls_reply.flags&CWW_F_READY)tls_ready=1;
        if(tls_reply.flags&CWW_F_CLOSED)tls_closed=1;
        tls_out_bytes=(u16)tls_reply.output_bytes;tls_out_at=0;
        tls_out_kind=(u16)tls_reply.flags;
        tls_drain=tls_job==CWW_TLS_CLOSE?0:tls_out_bytes!=0;
    }
    if(state!=S_HTTP)return;
    if(tls_tx_bytes){
        if((u16)(HOST.ticks-tls_tx_tick)>=9){
            (void)tcp_send(0x18,tls_tx,tls_tx_bytes,tls_tx_seq,rcv_nxt);tls_tx_tick=HOST.ticks;
        }
        return;
    }
    if(tls_out_at<tls_out_bytes){
        n=tls_out_bytes-tls_out_at;if(n>1024)n=1024;
        if(!webwork_result(tls_out_at,tls_chunk,n)){fail("Cannot read the HTTPS worker response.");return;}
        if(tls_out_kind&CWW_F_CIPHER){
            mem_copy(tls_tx,tls_chunk,n);tls_tx_seq=snd_nxt;tls_tx_bytes=n;tls_tx_tick=HOST.ticks;
            if(tcp_send(0x18,tls_tx,n,snd_nxt,rcv_nxt)){tls_tx_bytes=0;return;}
            snd_nxt+=n;
        }else if(tls_out_kind&CWW_F_PLAIN)http_consume(tls_chunk,n);
        tls_out_at+=n;return;
    }
    if(tls_ready&&!request_sent){
        if(!request_prepare()||!webwork_submit(CWW_TLS_WRITE,http_request,request_len,0)){
            fail("Cannot submit the HTTPS request.");return;
        }
        tls_job=CWW_TLS_WRITE;tls_busy=1;return;
    }
    if(tls_rx_bytes||tls_drain){
        tls_rx_sent=tls_rx_bytes;if(tls_rx_sent>1024)tls_rx_sent=1024;
        if(!webwork_submit(CWW_TLS_STEP,tls_rx,tls_rx_sent,0)){fail("Cannot continue the HTTPS transfer.");return;}
        tls_job=CWW_TLS_STEP;tls_busy=1;tls_drain=0;return;
    }
    /* RFC 2818 permits a securely framed HTTP body to complete at its
       Content-Length or final chunk even if TCP closes without close_notify.
       EOF-delimited bodies still require the authenticated TLS alert. */
    if(tls_ready&&http_framed_body_complete()){
        if(!tls_close_requested){
            if(!webwork_submit(CWW_TLS_CLOSE,0,0,0)){fail("Cannot send the HTTPS close_notify alert.");return;}
            tls_close_requested=1;tls_job=CWW_TLS_CLOSE;tls_busy=1;return;
        }
        if(tls_close_complete){
            (void)tcp_send(0x11,0,0,snd_nxt,rcv_nxt);++snd_nxt;state=S_DONE;tls_started=0;return;
        }
    }
    if(tls_fin&&!tls_closed){
        fail("HTTPS connection closed without TLS close_notify.");return;
    }
    if(tls_closed){
        if(!http_body_complete()){fail("HTTPS ended before the response was complete.");return;}
        (void)tcp_send(0x11,0,0,snd_nxt,rcv_nxt);++snd_nxt;state=S_DONE;tls_started=0;
    }
}
