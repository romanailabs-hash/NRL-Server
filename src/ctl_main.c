#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RBUF 65536

static int http_req(const char *method, const char *path, const char *body_str, const char *tok, char *resp, size_t rs) {
 char req[8192];
 char auth[256];
 if(tok) snprintf(auth,sizeof(auth),"Authorization: Bearer %s\r\n",tok);
 else auth[0]=0;
 
 int rl;
 if(body_str && body_str[0]) {
  rl=snprintf(req,sizeof(req),"%s %s HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n%s",
   method,path,strlen(body_str),auth,body_str);
 } else {
  rl=snprintf(req,sizeof(req),"%s %s HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n%sConnection: close\r\n\r\n",
   method,path,auth);
 }
 int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
 struct sockaddr_in a; a.sin_family=AF_INET; a.sin_port=htons(8080); a.sin_addr.s_addr=inet_addr("127.0.0.1");
 if(connect(fd,(struct sockaddr*)&a,sizeof(a))<0) { close(fd); return -1; }
 (void)write(fd,req,(size_t)rl);
 size_t t=0; ssize_t n; char buf[4096];
 while((n=read(fd,buf,sizeof(buf)))>0) { if(t+(size_t)n<rs-1) { memcpy(resp+t,buf,(size_t)n); t+=(size_t)n; } }
 resp[t]=0; close(fd);
 char *b=strstr(resp,"\r\n\r\n"); if(b) { b+=4; memmove(resp,b,strlen(b)+1); }
 return 0;
}

int main(int argc, char **argv) {
 const char *tok=getenv("NRL_SERVER_TOKEN");
 char resp[RBUF];
 if(argc<2) { fprintf(stderr,"Usage: %s <status|tick|run>\n",argv[0]); return 1; }
 if(strcmp(argv[1],"status")==0) {
  if(http_req("GET","/health",NULL,NULL,resp,sizeof(resp))==0) printf("Health: %s\n",resp);
  if(http_req("GET","/v1/stats",NULL,tok,resp,sizeof(resp))==0) printf("Stats: %s\n",resp);
 } else if(strcmp(argv[1],"tick")==0) {
  unsigned int s=(argc>2)?(unsigned int)atoi(argv[2]):10000;
  unsigned int sd=(argc>3)?(unsigned int)atoi(argv[3]):42;
  char body[128]; snprintf(body,sizeof(body),"{\"steps\":%u,\"input_seed\":%u}",s,sd);
  printf("Tick: "); fflush(stdout);
  if(http_req("POST","/v1/tick",body,tok,resp,sizeof(resp))==0) printf("%s\n",resp);
 } else if(strcmp(argv[1],"run")==0) {
  const char *cmd=(argc>2)?argv[2]:"/bin/echo hello";
  char body[512]; snprintf(body,sizeof(body),"{\"rootfs\":\"\",\"cmd\":[\"%s\"],\"timeout_ms\":5000,\"mem_mb\":128,\"cpu_quota_pct\":50,\"pids_max\":64}",cmd);
  printf("Run: "); fflush(stdout);
  if(http_req("POST","/v1/run",body,tok,resp,sizeof(resp))==0) printf("%s\n",resp);
 } else { fprintf(stderr,"Unknown: %s\n",argv[1]); return 1; }
 return 0;
}
