/* Included in file.c's stdio branch. One 16 KiB body slice per UI turn. */
    fetch_msg msg;
    if(!ctx->infile) {
        if(ctx->file_etag && ctx->file_etag==fdstat->st_mtime) {
            fetch_set_http_code(ctx->fetchh,304);msg.type=FETCH_NOTMODIFIED;
            fetch_file_send_callback(&msg,ctx);return;
        }
        if(fdstat->st_size<0 || fdstat->st_size>8*1024*1024) { fetch_file_process_error(ctx,413);return; }
        ctx->infile=fopen(ctx->path,"rb");
        if(!ctx->infile) { fetch_file_process_error(ctx,fetch_file_errno_to_http_code(errno));return; }
        ctx->file_stat=*fdstat;
        ctx->buffer=malloc(16384);
        if(!ctx->buffer) { msg.type=FETCH_ERROR;msg.data.error="Unable to allocate a file read buffer";fetch_file_send_callback(&msg,ctx);return; }
        fetch_set_http_code(ctx->fetchh,200);
        if(fetch_file_send_header(ctx,"Content-Type: %s",guit->fetch->filetype(ctx->path)) ||
           fetch_file_send_header(ctx,"Content-Length: %llu",(unsigned long long)fdstat->st_size) ||
           fetch_file_send_header(ctx,"ETag: \"%llu\"",(unsigned long long)fdstat->st_mtime))return;
    }
    size_t remaining=(size_t)ctx->file_stat.st_size-ctx->read_bytes;
    if(remaining) {
        size_t amount=remaining<16384?remaining:16384;
        size_t got=fread(ctx->buffer,1,amount,ctx->infile);
        if(!got) {
            msg.type=FETCH_ERROR;msg.data.error="Unexpected EOF or read error in local file";
            fetch_file_send_callback(&msg,ctx);return;
        }
        ctx->read_bytes+=got;
        ciuki_file_slices++;
        msg.type=FETCH_DATA;msg.data.header_or_data.buf=(const uint8_t *)ctx->buffer;msg.data.header_or_data.len=got;
        if(fetch_file_send_callback(&msg,ctx))return;
    }
    if(ctx->read_bytes<(size_t)ctx->file_stat.st_size) { ctx->pending=true;return; }
    msg.type=FETCH_FINISHED;fetch_file_send_callback(&msg,ctx);
