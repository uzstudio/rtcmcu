﻿#include "relay/RtpRelay.h"
#include "json.h"
#include "evhttp.h"
#include "../util/util.h"
#include "../util/log.h"
#include "cmd_protocol/proto.h"
//#include "rtp_backend_config.h"
#include "target.h"
#include "connection_manager/FlvConnectionManager.h"
//#include "connection_manager/rtp_uploader_config.h"
#include "config/RtpRelayConfig.h"
#include "config/RtpUploaderConfig.h"
#include "rtp_trans/rtp_trans_manager.h"
#include "common/TaskManager.h"

#define MAX_LEN_PER_READ (1024 * 128)

//////////////////////////////////////////////////////////////////////////

// TODO: zhangle, 系统中没有某个streamid时，应单独通知
class RtpPullClient {
public:
  RtpPullClient(struct event_base *ev_base, RtpPullTcpManager *mgr, const char *dispatch_host, unsigned short dispatch_port, const StreamId_Ext &streamid);
  ~RtpPullClient();
  int Start();
  const StreamId_Ext& streamid();
  bool IsUseless();

private:
  void GetNextNode();
  static void OnNextNode(struct evhttp_request* req, void* arg);
  void OnNextNodeImpl(int httpcode, const char *content, int len);

  int GetSdp(const char *next_node_host, unsigned short next_node_http_port);
  static void OnSdp(struct evhttp_request* req, void* arg);
  void OnSdpImpl(int httpcode, const char *content, int len);

  void ConnectRtpServer(const char *next_node_host, unsigned short next_node_rtp_port);

  void OnError();

  struct event_base *m_ev_base;
  std::string m_dispatch_host;
  unsigned short m_dispatch_port;
  StreamId_Ext m_streamid;
  struct evhttp_connection *m_http_con;
  std::string m_next_node_host;
  unsigned int m_next_node_rtp_port;   // TCP port
  struct event m_ev_socket;
  RtpPullTcpManager *m_manager;
  RtpConnection *m_rtp_connection;
  unsigned int m_last_retry_time;   // 单位秒
  unsigned int m_retry_delay;       // 单位秒
  unsigned int m_start_time;        // 单位秒
};

RtpPullClient::RtpPullClient(struct event_base *ev_base, RtpPullTcpManager *mgr, const char *dispatch_host, unsigned short dispatch_port, const StreamId_Ext &streamid) {
  m_ev_base = ev_base;
  m_dispatch_host = dispatch_host;
  m_dispatch_port = dispatch_port;
  m_streamid = streamid;
  m_http_con = NULL;
  m_manager = mgr;
  m_rtp_connection = NULL;
  m_last_retry_time = 0;
  m_retry_delay = 0;
  m_start_time = time(NULL);
}

RtpPullClient::~RtpPullClient() {
  if (m_http_con) {
    evhttp_connection_free(m_http_con);
    m_http_con = NULL;
  }
  if (m_rtp_connection) {
    RtpConnection::Destroy(m_rtp_connection);
    m_rtp_connection = NULL;
  }
}

int RtpPullClient::Start() {
  // 这个地方无需调用RtpConnection::Destroy()，
  // m_rtp_connection断开后RtpTcpManager会清理它，或超时时RTPTransManager会清理它
  m_rtp_connection = NULL;

  // 有一个重试机制：
  // 1. 首次，无delay
  // 2. 距离上次30s以上，通常表示这段时间成功过，那么再次重试时，也没有delay
  // 3. 其它情况，重试的delay为1s、2s、4s、8s、8s、8s ...
  if (!m_last_retry_time || time(NULL) - m_last_retry_time > 30) {
    m_retry_delay = 0;
    GetNextNode();
  }
  else {
    unsigned int delay = m_retry_delay * 2;
    if (delay <= 0) {
      delay = 1;
    }
    else if (delay > 8) {
      delay = 8;
    }
    m_retry_delay = delay;
    TaskManger::Instance()->PostTask(std::bind(&RtpPullClient::GetNextNode, this), m_retry_delay);
  }
  return 0;
}

const StreamId_Ext& RtpPullClient::streamid() {
  return m_streamid;
}

// 如果一路流没有人拉流（即同时没有rtp和flv拉取），那么pull应该断开
bool RtpPullClient::IsUseless() {
  // 在最开始时的10s内，强制认为是有效的
  // （因为此时rtp pull并没有创建RTPTrans, RTPTransManager::HasPlayer()必然是false）
  return !LiveConnectionManager::Instance()->HasPlayer(m_streamid)
    && !RTPTransManager::Instance()->HasPlayer(m_streamid)
    && (time(NULL) - m_start_time > 10);
}

void RtpPullClient::GetNextNode() {
  m_last_retry_time = time(NULL);
  m_next_node_host = "127.0.0.1";
  m_next_node_rtp_port = 8142;
  GetSdp(m_next_node_host.c_str(), 8102);
}

int RtpPullClient::GetSdp(const char *next_node_host, unsigned short next_node_http_port) {
  //INF("got sdpinfo ip %s sdpport %d streamid %s", util_ip2str(client->ip),
  //  client->sdpport, client->stream_id.unparse().c_str());
  if (m_http_con) {
    evhttp_connection_free(m_http_con);
    m_http_con = NULL;
  }

  m_http_con = evhttp_connection_base_new(m_ev_base, NULL, next_node_host, next_node_http_port);
  evhttp_connection_set_timeout(m_http_con, 3);
  evhttp_connection_set_retries(m_http_con, 3);

  // TODO: zhangle, req maybe need delete
  struct evhttp_request *req = evhttp_request_new(&RtpPullClient::OnSdp, this);
  evhttp_add_header(req->output_headers, "User-Agent", "ForwardClientRtpTCP");
  evhttp_add_header(req->output_headers, "Host", next_node_host);

  char path[256];
  sprintf(path, "/download/sdp?streamid=%s", m_streamid.unparse().c_str());
  DBG("req sdp url %s streamid %s", path, m_streamid.unparse().c_str());
  int ret = 0;
  if ((ret = evhttp_make_request(m_http_con, req, EVHTTP_REQ_GET, path)) != 0) {
    ERR("get sdpinfo error streamid %s ret %d", m_streamid.unparse().c_str(), ret);
    OnError();
    return -1;
  }
  return 0;
}

void RtpPullClient::OnSdp(struct evhttp_request* req, void* arg) {
  int httpcode = 0;
  const char *content = NULL;
  int len = 0;

  if (req) {
    httpcode = req->response_code;
    content = reinterpret_cast<const char*>(EVBUFFER_DATA(req->input_buffer));
    len = EVBUFFER_LENGTH(req->input_buffer);
  }

  if (arg) {
    RtpPullClient *pThis = (RtpPullClient*)arg;
    pThis->OnSdpImpl(httpcode, content, len);
  }
}

void RtpPullClient::OnSdpImpl(int httpcode, const char *content, int len) {
  if (httpcode == HTTP_OK && content && len > 0) {
    std::string sdp(content, len);
    if (RTPTransManager::Instance()->set_sdp_str(m_streamid, sdp) >= 0) {
      ConnectRtpServer(m_next_node_host.c_str(), m_next_node_rtp_port);
      return;
    }
  }

  ERR("put sdp error, streamid=%s", m_streamid.unparse().c_str());
  OnError();
}

void RtpPullClient::ConnectRtpServer(const char *next_node_host, unsigned short next_node_rtp_port) {
  DBG("connect ip %s, player port %d, streamid %s", next_node_host,
    (int)next_node_rtp_port, m_streamid.unparse().c_str());

  struct sockaddr_in remote;
  memset(&remote, 0, sizeof(remote));
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = inet_addr(next_node_host);
  remote.sin_port = htons(next_node_rtp_port);
  int fd_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_sock < 0) {
    ERR("do_connect: create socket error, errno=%d, err=%s, streamid %s", errno, strerror(errno), m_streamid.unparse().c_str());
    OnError();
    return;
  }
  util_set_nonblock(fd_sock, TRUE);

  int sock_rcv_buf_sz = 512 * 1024;
  setsockopt(fd_sock, SOL_SOCKET, SO_RCVBUF,
    (void*)&sock_rcv_buf_sz, sizeof(sock_rcv_buf_sz));
  setsockopt(fd_sock, SOL_SOCKET, SO_SNDBUF,
    (void*)&sock_rcv_buf_sz, sizeof(sock_rcv_buf_sz));

  int ret = connect(fd_sock, (struct sockaddr*) &remote, sizeof(remote));
  if (ret < 0) {
    if ((errno != EINPROGRESS) && (errno != EINTR)) {
      WRN("connect error: %d %s streamid %s", errno, strerror(errno), m_streamid.c_str());
      OnError();
      return;
    }
  }

  RtpConnection *c = m_manager->CreateConnection(&remote, fd_sock);
  m_rtp_connection = c;
  c->streamid = m_streamid;
  c->type = RtpConnection::CONN_TYPE_UPLOADER;
  c->relay_pull = true;
  //INF("uploader accepted. socket=%d, remote=%s:%d", connection->fd_socket, connection->remote_ip,
  //  (int)connection->remote.sin_port);

  buffer *reqbuf = buffer_init(256);
  rtp_d2p_req_state req;
  req.payload_type = 2;
  memcpy(req.streamid, &m_streamid, sizeof(m_streamid));
  memset(req.token, 0, sizeof(req.token));
  memcpy(req.token, "98765", 5);
  req.version = 1;
  memset(req.useragent, 0, sizeof(req.useragent));
  const char *user_agent = "ForwardClientRtpTCP";
  memcpy(req.useragent, user_agent, strlen(user_agent));
  encode_rtp_d2p_req_state(&req, reqbuf);
  buffer_append(c->wb, reqbuf);
  buffer_free(reqbuf);

  RtpRelayConfig* cfg_rtp_backend = (RtpRelayConfig*)ConfigManager::get_inst_config_module("rtp_relay");
  const RTPTransConfig* cfg_rtp_trans = cfg_rtp_backend->get_rtp_conf();
  RTPTransManager::Instance()->_open_trans(c, const_cast<RTPTransConfig*>(cfg_rtp_trans));
}

void RtpPullClient::OnError() {
  // 重试
  Start();
}

//////////////////////////////////////////////////////////////////////////

RtpPullTcpManager::RtpPullTcpManager() {
  m_ev_base = NULL;
  m_ev_timer = NULL;
}

RtpPullTcpManager::~RtpPullTcpManager() {
  if (m_ev_timer) {
    event_free(m_ev_timer);
    m_ev_timer = NULL;
  }
}

int RtpPullTcpManager::Init(struct event_base *ev_base) {
  m_ev_base = ev_base;

  m_ev_timer = event_new(m_ev_base, -1, EV_PERSIST, OnPullTimer, this);
  struct timeval tv;
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  event_add(m_ev_timer, &tv);
  return 0;
}

void RtpPullTcpManager::StartPull(const StreamId_Ext& streamid) {
  if (m_clients.find(streamid) != m_clients.end()) {
    return;
  }
  RtpPullClient *client = new RtpPullClient(m_ev_base, this, "xxx", 0, streamid);
  m_clients[streamid] = client;
  client->Start();
}

void RtpPullTcpManager::StopPull(const StreamId_Ext& streamid) {
  auto it = m_clients.find(streamid);
  if (it != m_clients.end()) {
    RtpPullClient* client = it->second;
    m_clients.erase(it);
    delete client;
  }
}

void RtpPullTcpManager::OnConnectionClosed(RtpConnection *c) {
  RtpTcpManager::OnConnectionClosed(c);

  auto it = m_clients.find(c->streamid);
  if (it != m_clients.end()) {
    // 未调用RtpPullTcpManager::StopPull时，RtpConnection的断开均进行重试
    RtpPullClient* client = it->second;
    client->Start();
  }
}

void RtpPullTcpManager::OnPullTimer(evutil_socket_t fd, short flag, void *arg) {
  RtpPullTcpManager *pThis = (RtpPullTcpManager*)arg;
  pThis->OnPullTimerImpl(fd, flag, arg);
}

void RtpPullTcpManager::OnPullTimerImpl(evutil_socket_t fd, short flag, void *arg) {
  std::set<RtpPullClient*> trash;
  for (auto it = m_clients.begin(); it != m_clients.end(); it++) {
    RtpPullClient *c = it->second;
    if (c->IsUseless()) {
      trash.insert(c);
    }
  }
  for (auto it = trash.begin(); it != trash.end(); it++) {
    RtpPullClient *c = *it;
    StopPull(c->streamid());
  }
}

//////////////////////////////////////////////////////////////////////////

// IODO: zhangle, 需要注意循环push的问题
class RtpPushClient {
public:
  RtpPushClient(struct event_base *ev_base, RtpPushTcpManager *mgr, const char *dispatch_host, unsigned short dispatch_port, const StreamId_Ext &streamid);
  ~RtpPushClient();
  int Start();

private:
  int GetNextNode();
  static void OnNextNode(struct evhttp_request* req, void* arg);
  void OnNextNodeImpl(int httpcode, const char *content, int len);

  int PutSdp(const char *next_node_host, unsigned short next_node_http_port);
  static void OnSdp(struct evhttp_request* req, void* arg);
  void OnSdpImpl(int httpcode, const char *content, int len);

  void ConnectRtpServer(const char *next_node_host, unsigned short next_node_rtp_port);

  void OnError();

  struct event_base *m_ev_base;
  std::string m_dispatch_host;
  unsigned short m_dispatch_port;
  StreamId_Ext m_streamid;
  struct evhttp_connection *m_http_con;
  std::string m_next_node_host;
  unsigned int m_next_node_rtp_port;   // TCP port
  struct event m_ev_socket;
  RtpPushTcpManager *m_manager;
  RtpConnection *m_rtp_connection;
};

RtpPushClient::RtpPushClient(struct event_base *ev_base, RtpPushTcpManager *mgr, const char *dispatch_host, unsigned short dispatch_port, const StreamId_Ext &streamid) {
  m_ev_base = ev_base;
  m_dispatch_host = dispatch_host;
  m_dispatch_port = dispatch_port;
  m_streamid = streamid;
  m_http_con = NULL;
  m_manager = mgr;
  m_rtp_connection = NULL;
}

RtpPushClient::~RtpPushClient() {
  if (m_http_con) {
    evhttp_connection_free(m_http_con);
    m_http_con = NULL;
  }
  if (m_rtp_connection) {
    RtpConnection::Destroy(m_rtp_connection);
    m_rtp_connection = NULL;
  }
}

int RtpPushClient::Start() {
  return GetNextNode();
}

int RtpPushClient::GetNextNode() {
  // TODO: zhangle, is just test code
  RTPUploaderConfig *config = (RTPUploaderConfig *)ConfigManager::get_inst_config_module("rtp_uploader");
  if (NULL == config) {
    ERR("rtp uploader failed to get corresponding config information.");
    return -1;
  }
  if (config->listen_tcp_port == 8143) {
    return -1;
  }

  m_next_node_host = "127.0.0.1";
  m_next_node_rtp_port = 8143;
  PutSdp(m_next_node_host.c_str(), 8103);
  return 0;
}

int RtpPushClient::PutSdp(const char *next_node_host, unsigned short next_node_http_port) {
  std::string sdp = RTPTransManager::Instance()->get_sdp_str(m_streamid);
  if (sdp.empty()) {
    return -1;
  }

  if (m_http_con) {
    evhttp_connection_free(m_http_con);
    m_http_con = NULL;
  }
  m_http_con = evhttp_connection_base_new(m_ev_base, NULL, next_node_host, next_node_http_port);
  evhttp_connection_set_timeout(m_http_con, 3);
  evhttp_connection_set_retries(m_http_con, 3);

  // TODO: zhangle, req maybe need delete
  struct evhttp_request *req = evhttp_request_new(&RtpPushClient::OnSdp, this);
  evhttp_add_header(req->output_headers, "User-Agent", "ForwardClientRtpTCP");
  evhttp_add_header(req->output_headers, "Host", next_node_host);

  char path[256];
  sprintf(path, "/upload/sdp?streamid=%s", m_streamid.unparse().c_str());
  DBG("put sdp url %s streamid %s", path, m_streamid.unparse().c_str());
  evbuffer_add(req->output_buffer, sdp.c_str(), sdp.length());
  int ret = 0;
  if ((ret = evhttp_make_request(m_http_con, req, EVHTTP_REQ_POST, path)) != 0) {
    ERR("get sdpinfo error streamid %s ret %d", m_streamid.unparse().c_str(), ret);
    return -1;
  }
  return 0;
}

void RtpPushClient::OnSdp(struct evhttp_request* req, void* arg) {
  int httpcode = 0;
  const char *content = NULL;
  int len = 0;

  if (req) {
    httpcode = req->response_code;
    content = reinterpret_cast<const char*>(EVBUFFER_DATA(req->input_buffer));
    len = EVBUFFER_LENGTH(req->input_buffer);
  }

  if (arg) {
    RtpPushClient *pThis = (RtpPushClient*)arg;
    pThis->OnSdpImpl(httpcode, content, len);
  }
}

void RtpPushClient::OnSdpImpl(int httpcode, const char *content, int len) {
  if (httpcode == HTTP_OK) {
    ConnectRtpServer(m_next_node_host.c_str(), m_next_node_rtp_port);
    return;
  }

  ERR("put sdp error, streamid=%s", m_streamid.unparse().c_str());
  OnError();
}

void RtpPushClient::ConnectRtpServer(const char *next_node_host, unsigned short next_node_rtp_port) {
  DBG("connect ip %s, player port %d, streamid %s", next_node_host,
    (int)next_node_rtp_port, m_streamid.unparse().c_str());

  struct sockaddr_in remote;
  memset(&remote, 0, sizeof(remote));
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = inet_addr(next_node_host);
  remote.sin_port = htons(next_node_rtp_port);
  int fd_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_sock < 0) {
    ERR("do_connect: create socket error, errno=%d, err=%s, streamid %s", errno, strerror(errno), m_streamid.unparse().c_str());
    OnError();
    return;
  }
  util_set_nonblock(fd_sock, TRUE);

  int sock_rcv_buf_sz = 512 * 1024;
  setsockopt(fd_sock, SOL_SOCKET, SO_RCVBUF,
    (void*)&sock_rcv_buf_sz, sizeof(sock_rcv_buf_sz));
  setsockopt(fd_sock, SOL_SOCKET, SO_SNDBUF,
    (void*)&sock_rcv_buf_sz, sizeof(sock_rcv_buf_sz));

  int ret = connect(fd_sock, (struct sockaddr*) &remote, sizeof(remote));
  if (ret < 0) {
    if ((errno != EINPROGRESS) && (errno != EINTR)) {
      WRN("connect error: %d %s streamid %s", errno, strerror(errno), m_streamid.c_str());
      OnError();
      return;
    }
  }

  if (m_rtp_connection) {
    RtpConnection::Destroy(m_rtp_connection);
    m_rtp_connection = NULL;
  }
  RtpConnection *c = m_manager->CreateConnection(&remote, fd_sock);
  m_rtp_connection = c;
  c->streamid = m_streamid;
  c->type = RtpConnection::CONN_TYPE_PLAYER;

  buffer *reqbuf = buffer_init(256);
  rtp_u2r_req_state req;
  req.payload_type = 2;
  memcpy(req.streamid, &m_streamid, sizeof(m_streamid));
  memset(req.token, 0, sizeof(req.token));
  memcpy(req.token, "98765", 5);
  req.version = 1;
  req.user_id = 2;
  if (encode_rtp_u2r_req_state(&req, reqbuf)) {
    buffer_free(reqbuf);
    OnError();
    return;
  }
  buffer_append(c->wb, reqbuf);
  buffer_free(reqbuf);

  RtpRelayConfig* cfg_rtp_backend = (RtpRelayConfig*)ConfigManager::get_inst_config_module("rtp_relay");
  const RTPTransConfig* cfg_rtp_trans = cfg_rtp_backend->get_rtp_conf();
  RTPTransManager::Instance()->_open_trans(c, const_cast<RTPTransConfig*>(cfg_rtp_trans));
}

void RtpPushClient::OnError() {
  // TODO: zhangle
  m_manager->StopPush(m_streamid);
}

//////////////////////////////////////////////////////////////////////////

int RtpPushTcpManager::Init(struct event_base * ev_base) {
  m_ev_base = ev_base;
  return 0;
}

void RtpPushTcpManager::StartPush(const StreamId_Ext& streamid) {
  if (m_clients.find(streamid) != m_clients.end()) {
    return;
  }
  RtpPushClient *client = new RtpPushClient(m_ev_base, this, "xxx", 0, streamid);
  m_clients[streamid] = client;
  client->Start();
}

void RtpPushTcpManager::StopPush(const StreamId_Ext& streamid) {
  auto it = m_clients.find(streamid);
  if (it != m_clients.end()) {
    RtpPushClient* client = it->second;
    m_clients.erase(it);
    delete client;
  }
}
