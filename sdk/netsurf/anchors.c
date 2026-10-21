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
static const char *prefix="ciuki",*arrays="ca";
static void array(unsigned i,const char *name,const unsigned char *p,size_t n)
{
    printf("static const unsigned char %s_%u_%s[] = {",arrays,i,name);
    for(size_t j=0;j<n;j++)printf("%s%u",j?",":"",p[j]);
    puts("};");
}
int main(int argc,char **argv)
{
    /* argv[2]: symbol prefix (f6-18 test anchors: "ciuki_test" names the
     * table ciuki_test_anchors_table, CIUKI_TEST_ANCHOR_COUNT and ciuki_test_ca_* arrays). */
    static char table[64],macro[64],array_prefix[64];
    if(argc!=2 && argc!=3)return 2;
    if(argc==3){
        if(strlen(argv[2])>32)return 2;
        prefix=argv[2];
        snprintf(array_prefix,sizeof(array_prefix),"%s_ca",prefix);arrays=array_prefix;
        snprintf(table,sizeof(table),"%s_anchors_table",prefix);
        for(size_t i=0;prefix[i];i++)macro[i]=(char)((prefix[i]>='a'&&prefix[i]<='z')?prefix[i]-32:prefix[i]);
        macro[strlen(prefix)]=0;strcat(macro,"_ANCHOR_COUNT");
    } else { strcpy(table,"ciuki_anchors");strcpy(macro,"CIUKI_ANCHOR_COUNT"); }
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
    printf("static const br_x509_trust_anchor %s[] = {\n",table);
    for(unsigned i=0;i<count;i++){
        printf("{{(unsigned char *)%s_%u_dn,%zu},BR_X509_TA_CA,{%u,{",arrays,i,sizes[i][0],types[i]);
        if(types[i]==BR_KEYTYPE_RSA)printf(".rsa={(unsigned char *)%s_%u_n,%zu,(unsigned char *)%s_%u_e,%zu}",arrays,i,sizes[i][1],arrays,i,sizes[i][2]);
        else printf(".ec={%d,(unsigned char *)%s_%u_q,%zu}",curves[i],arrays,i,sizes[i][1]);
        puts("}}},");
    }
    printf("};\n#define %s %u\n",macro,count);
    return 0;
}
