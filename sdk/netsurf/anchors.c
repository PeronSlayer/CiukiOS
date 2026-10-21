/* Offline trust-anchor generator using the pinned BearSSL decoder.
 * SPDX-License-Identifier: MIT */
#include <bearssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
static unsigned char der[65536],dn[16384];
static size_t der_bytes,dn_bytes;
static int overflow;
static void collect(void *ctx,const void *s,size_t n)
{
    int name=ctx!=NULL;size_t *bytes=name?&dn_bytes:&der_bytes;
    size_t cap=name?sizeof(dn):sizeof(der);unsigned char *out=name?dn:der;
    if(n>cap-*bytes){overflow=1;return;}memcpy(out+*bytes,s,n);*bytes+=n;
}
static void array(unsigned i,const char *name,const unsigned char *p,size_t n)
{
    printf("static const unsigned char ca_%u_%s[] = {",i,name);
    for(size_t j=0;j<n;j++)printf("%s%u",j?",":"",p[j]);
    puts("};");
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    FILE *in=fopen(argv[1],"rb");if(!in)return 2;
    br_pem_decoder_context pem;br_pem_decoder_init(&pem);
    unsigned count=0;unsigned char types[512]={0};size_t sizes[512][3];int curves[512]={0};
    puts("/* Generated offline from the pinned CA bundle with BearSSL 0.6. */\n#include <bearssl.h>");
    int c;bool active=false;
    while((c=fgetc(in))!=EOF){
        unsigned char b=(unsigned char)c;
        if(br_pem_decoder_push(&pem,&b,1)!=1)return 3;
        int event=br_pem_decoder_event(&pem);
        if(event==BR_PEM_BEGIN_OBJ){
            if(strcmp(br_pem_decoder_name(&pem),"CERTIFICATE"))return 3;
            der_bytes=dn_bytes=0;overflow=0;active=true;
            br_pem_decoder_setdest(&pem,collect,NULL);
        } else if(event==BR_PEM_END_OBJ){
            if(!active || overflow || count==512)return 3;
            active=false;
            br_x509_decoder_context decoder;
            br_x509_decoder_init(&decoder,collect,(void *)1);
            br_x509_decoder_push(&decoder,der,der_bytes);
            br_x509_pkey *key=br_x509_decoder_get_pkey(&decoder);
            if(!key || overflow || !dn_bytes || !br_x509_decoder_isCA(&decoder))return 4;
            array(count,"dn",dn,dn_bytes);sizes[count][0]=dn_bytes;
            types[count]=key->key_type;
            if(key->key_type==BR_KEYTYPE_RSA){
                array(count,"n",key->key.rsa.n,key->key.rsa.nlen);array(count,"e",key->key.rsa.e,key->key.rsa.elen);
                sizes[count][1]=key->key.rsa.nlen;sizes[count][2]=key->key.rsa.elen;
            } else if(key->key_type==BR_KEYTYPE_EC){
                array(count,"q",key->key.ec.q,key->key.ec.qlen);sizes[count][1]=key->key.ec.qlen;curves[count]=key->key.ec.curve;
            } else return 4;
            count++;
        } else if(event==BR_PEM_ERROR)return 3;
    }
    if(ferror(in)||active||!count)return 3;
    fclose(in);
    puts("static const br_x509_trust_anchor ciuki_anchors[] = {");
    for(unsigned i=0;i<count;i++){
        printf("{{(unsigned char *)ca_%u_dn,%zu},BR_X509_TA_CA,{%u,{",i,sizes[i][0],types[i]);
        if(types[i]==BR_KEYTYPE_RSA)printf(".rsa={(unsigned char *)ca_%u_n,%zu,(unsigned char *)ca_%u_e,%zu}",i,sizes[i][1],i,sizes[i][2]);
        else printf(".ec={%d,(unsigned char *)ca_%u_q,%zu}",curves[i],i,sizes[i][1]);
        puts("}}},");
    }
    printf("};\n#define CIUKI_ANCHOR_COUNT %u\n",count);
    return 0;
}
