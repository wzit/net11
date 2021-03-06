#include <string>
#include <map>
#include <algorithm>

#include <iostream>
#include <sys/stat.h>
#include <stdio.h>

#include "util.hpp"
#include "tcp.hpp"
#include "base64.hpp"
#include "sha1.hpp"

namespace net11 {
	namespace http {
		class connection;
		class response;
		class websocket_response;
		class websocket_sink;
		class websocket;

		response* make_text_response(int code,std::string &data);

		class response {
			friend connection;
			friend websocket_response;
			friend response* make_stream_response(int code,std::function<bool(buffer &data)> prod);

			std::map<std::string,std::string> head;
			std::function<bool(buffer &)> prod;

			response(){}
		protected:
			int code;
			virtual void produce_headers(connection &conn);
			virtual bool produce(connection &conn);
		public:
			response* set_header(const std::string &k,const std::string &v) {
				head[k]=v;
				return this;
			}
			virtual ~response() {}
		};

		class connection : public net11::tcp::connection {
			friend std::function<net11::tcp::connection*()> make_server(std::function<response*(connection &conn)> route);
			friend response;
			std::shared_ptr<sink> reqlinesink;
			std::string reqline[3];

			std::shared_ptr<sink> headsink;
			std::map<std::string,std::string> headers;

			std::function<response*(connection &conn)> router;

			connection(
				std::function<response*(connection &conn)> in_router
			):router(in_router) {
				reqlinesink=std::shared_ptr<sink>(new net11::line_parser_sink("\r\n",4096,[this](std::string &l){
					bool in_white=false;
					int outidx=0;
					reqline[0].resize(0);
					reqline[1].resize(0);
					reqline[2].resize(0);
					for (int i=0;i<l.size();i++) {
						char c=l[i];
						if (isspace(c)) {
							in_white=true;
						} else {
							if (in_white && outidx<2) {
								outidx++;
							}
							reqline[outidx].push_back(c);
							in_white=false;
						}
					}
					if (outidx>0 && reqline[0].size() && reqline[1].size()) {
						this->current_sink=headsink;
						return true;
					} else {
						// TODO: error handling?!
						return false;
					}
				}));
				headsink=std::shared_ptr<sink>(new header_parser_sink(128*1024,tolower,
					[this](std::string &k,std::string &v){
#ifdef NET11_VERBOSE
						std::cout<<"HeadKey=["<<k<<"] HeadValue=["<<v<<"]\n";
#endif
						headers[k]=v;
						return true;
					},
					[this](const char *err) {
#ifdef NET11_VERBOSE
						std::cout<<"req:"<<reqline[0]<<" url:"<<reqline[1]<<" ver:"<<reqline[2]<<"\n";
#endif
						// reset our sink early in case the router and/or response wants to hijack it
						this->current_sink=reqlinesink;
						// TODO: urlencodings?
						response *r=router(*this);
						if (!r) {
							//std::map<std::string,std::string> head; //{{"connection","close"}};
							std::string msg="Error 404, "+url()+" not found";
							r=make_text_response(404,msg);
						}
						// TODO: make sure that connection lines are there?
						bool rv=r->produce(*this);
						delete r;
						reqline[0].resize(0);
						reqline[1].resize(0);
						reqline[2].resize(0);
						headers.clear();
						return rv;
					}
				));
				current_sink=reqlinesink;
			}
			virtual ~connection() {
				//printf("Killed http connection\n");
			}
			// small helper for the template expansion of has_headers
			bool has_headers() {
				return true;
			}
		public:
			std::string& method() {
				return reqline[0];
			}
			std::string& url() {
				return reqline[1]; // TODO should it be pre-decoded?
			}
			std::string* header(const char *in_k) {
				std::string k(in_k);
				return header(k);
			}
			std::string* header(std::string &k) {
				auto f=headers.find(k);
				if (f!=headers.end())
					return &f->second;
				else
					return 0;
			}
			std::string lowerheader(const char *k) {
				std::string ok(k);
				return lowerheader(ok);
			}
			std::string lowerheader(std::string &k) {
				std::string out;
				auto f=headers.find(k);
				if (f!=headers.end()) {
					for (int i=0;i<f->second.size();i++)
						out.push_back(tolower(f->second[i]));
				}
				return out;
			}
			bool has_header(std::string &k) {
				return headers.count(k)!=0;
			}
			bool has_header(const char *p) {
#ifdef NET11_VERBOSE
				std::cout
					<<"HasHeader:"
					<<p
					<<" -> "
					<<(headers.count(std::string(p))!=0)
					<<"\n";
#endif
				return headers.count(std::string(p))!=0;
			}
			template<typename HEAD,typename... REST>
			bool has_headers(HEAD head,REST... rest) {
				return has_header(head)&&has_headers(rest...);
			}
		};

		std::function<net11::tcp::connection*()> make_server(std::function<response*(connection &conn)> route) {
			// now create a connection spawn function
			return [route]() {
				return (net11::tcp::connection*)new connection(route);
			};
		};

		response* make_stream_response(int code,std::function<bool(buffer &data)> prod) {
			auto out=new response();
			out->code=code;
			out->prod=prod;
			return out;
		}

		response* make_blob_response(int code,std::vector<char> &in_data) {
			auto rv=make_stream_response(code,make_data_producer(in_data));
			rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
			return rv;
		}
		response* make_blob_response(int code,std::vector<char> in_data) {
			auto rv=make_stream_response(code,make_data_producer(in_data));
			rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
			return rv;
		}
		response* make_text_response(int code,std::string &in_data) {
			auto rv=make_stream_response(code,make_data_producer(in_data));
			rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
			return rv;
		}
		response* make_text_response(int code,std::string in_data) {
			auto rv=make_stream_response(code,make_data_producer(in_data));
			rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
			return rv;
		}

		void response::produce_headers(connection &conn) {
			std::string resline="HTTP/1.1 "+std::to_string(code)+" some message\r\n";
			for(auto kv:head) {
				resline=resline+kv.first+": "+kv.second+"\r\n";
			}
			resline+="\r\n";
			conn.producers.push_back(make_data_producer(resline));
		}

		bool response::produce(connection &conn) {
			produce_headers(conn);
			if (head.count("content-length")) {
				conn.producers.push_back(prod);
			} else {
				abort(); //conn.producers.push_back([this](
			}
			return true;
		}

		class websocket : public std::enable_shared_from_this<websocket> {
			friend websocket_sink;
			std::weak_ptr<connection> conn;
			int input_type=-1;
			websocket(std::weak_ptr<connection> in_conn):conn(in_conn),input_type(-1) {}
		public:
			int get_input_type() {
				return input_type;
			}
			static const int text=1;
			static const int binary=2;
			bool send(int ty,const char *data,uint64_t sz) {
				if (auto c=conn.lock()) {
					int shift;
					int firstsize;
					if (sz<126) {
						shift=0;
						firstsize=sz;
					} else if (sz<65536) {
						shift=16;
						firstsize=126;
					} else {
						shift=64;
						firstsize=127;
					}
					std::shared_ptr<buffer> b(new buffer(2+(shift/8)+sz));
					b->produce(0x80|ty);
					b->produce(firstsize);
					while(shift) {
						shift-=8;
						b->produce( (sz>>shift)&0xff );
					}
					std::memcpy(b->to_produce(),data,sz);
					b->produced(sz);
					c->producers.push_back([b](buffer& out) {
						out.produce(*b);
						return 0!=b->usage();
					});
					return true;
				} else {
					// Sending to killed connection!
					return false;
				}
			}
			bool send(const std::string& data) {
				return send(text,data.data(),data.size());
			}
			bool send(const std::vector<char>& data) {
				return send(binary,data.data(),data.size());
			}
		};

		class websocket_sink : public sink {
			friend websocket_response;
			enum wsstate {
				firstbyte=0,
				sizebyte,
				sizeextra,
				maskbytes,
				bodybytes
			};
			wsstate state;
			uint8_t info;
			uint64_t count;
			uint64_t size;
			bool want_mask;
			uint32_t mask;
			//std::vector<char> data;
			
			char control_data[125];
			
			bool endit() {
				bool fin=info&0x80;
				if ((info&0xf)<=2) {
					int type=info&0xf;
					if (websock->input_type==-1) {
						if (type==0)
							return false; // must get a new type!
						websock->input_type=type;
					} else {
						if (type!=0)
							return false; // already a set type!
					}
					if (!packet_end(fin,info&0xf))
						return false;
					if (fin)
						websock->input_type=-1;
				} else {
					// processing for non-data packets.
					if ((info&0xf)==8) {
						websock->send(8,control_data,0);
						return false;
					} else if ((info&0xf)==9) {
						// got a ping, need to do pong
						websock->send(10,control_data,size);
					} else if ((info&0xf)==10) {
						// 10, we ignore pongs..
					} else {
						// unknown packet type
						return false;
					}
				}
				state=firstbyte;
				return true;
			}
			bool advance() {
				count=0;
				if (state==sizebyte || state==sizeextra) {
					if (want_mask) {
						state=maskbytes;
						return true;
					}
				}
				state=bodybytes;
				bool ok=true;
				if (info&0x70)
					return false; // Not allowed to use reserved bits
				if (!(info&0x80) && ((info&0xf)>7))
					return false;
				if ((info&0xf)<=2) {
					ok&=packet_start(info&0x80,info&0xf,size);
				} else {
					if (size>sizeof(control_data)) {
						return false;
					}
					switch(info&0xf) {
					case 8 : // close
						ok=true;
						break;
					case 9 : case 10 : // ping-pong
						ok=true;
						break;
					default:
						ok=false; // do not know how to handle packet
						break;
					}
				}
				if (size==0) {
					return ok&endit();
				} else {
					return ok;
				}
			}
			std::shared_ptr<websocket> websock;
		protected:
			websocket_sink(std::weak_ptr<connection> conn):state(firstbyte),websock(new websocket(conn)) {}
		public:
			virtual bool drain(buffer &buf) {
				while(buf.usage()) {
					switch(state) {
					case firstbyte :
						info=buf.consume();
						state=sizebyte;
						mask=0;
						count=0;
						continue;
					case sizebyte :
						{
							int tmp=buf.consume();
							want_mask=tmp&0x80;
							if ((tmp&0x7f)<126) {
								size=tmp&0x7f;
								if (!advance())
									return false;
								continue;
							} else {
								if((tmp&0x7f)==126) {
									count=6; // skip "initial" 6 bytes since we only want a 2 byte size
								} else {
									count=0;
								}
								size=0;
								state=sizeextra;
								continue;
							}
						}
					case sizeextra:
						size=(size<<8)|(buf.consume()&0xff);
						if ((++count)==8) {
							if (!advance())
								return false;
						}
						continue;
					case maskbytes :
						mask=(mask<<8)|(buf.consume()&0xff);
						if ((++count)==4) {
							if (!advance())
								return false;
						}
						continue;
					case bodybytes :
						{
							int b=(buf.consume()^( mask >> ( 8*(3^(count&3))) ))&0xff;
							if ((info&0xf)<=2) {
								packet_data(b);
							} else {
								control_data[count]=b;
							}
							if (++count==size) {
								//printf("End of packet (%d)\n",info);
								if (!endit())
									return false;
							}
							continue;
						}
					}
				}
				return true;
			}
			std::weak_ptr<websocket> get_websocket() {
				return websock;
			}
			virtual bool packet_start(bool fin,int type,uint64_t size)=0;
			virtual void packet_data(char c)=0;
			virtual bool packet_end(bool fin,int type)=0;
		};

		class websocket_response : public response {
			friend response* make_websocket(connection &c,std::shared_ptr<websocket_sink> wssink);
			std::shared_ptr<websocket_sink> sink;
			websocket_response(std::shared_ptr<websocket_sink> in_sink):sink(in_sink) {
				code=101;
			}
			bool produce(connection &conn) {
				produce_headers(conn);
				conn.current_sink=sink;
				return true;
			}
		public:
			std::weak_ptr<websocket> get_websocket() {
				return sink->websock;
			}
		};

		response* make_websocket(connection &c,std::shared_ptr<websocket_sink> wssink) {
			bool has_heads=c.has_headers(
				"connection",
				"upgrade",
				//"origin",
				"sec-websocket-version",
				"sec-websocket-key");
			//printf("Has heads?:%d\n",has_heads);
			if (!has_heads)
				return 0;
			if (c.lowerheader("connection")!="upgrade") {
				return 0;
			}
			if (c.lowerheader("upgrade")!="websocket") {
				return 0;
			}
			if (*c.header("sec-websocket-version")!="13") {
				return 0;
			}
			// hash the key and guid to know the response hash
			net11::sha1 s;
			char hash[20];
			s.addbytes(*c.header("sec-websocket-key"));
			s.addbytes("258EAFA5-E914-47DA-95CA-C5AB0DC85B11",36);
			s.digest(hash);
			// base64 encode the hash into a response token
			std::string rkey;
			net11::base64encoder().encode(rkey,hash,20,true);
			// now setup the response!
			websocket_response *ws=new websocket_response(wssink);
			ws->set_header("Upgrade","websocket");
			ws->set_header("Connection","upgrade");
			ws->set_header("Sec-Websocket-Accept",rkey);
			//ws.set_header("sec-websocket-protocol") // proto?!
			return ws;
		}

		response* make_websocket(connection &c,int max_packet,std::function<bool(websocket &s,std::vector<char>&)> on_data) {
			struct websocket_packet_sink : public websocket_sink {
				int max_packet;
				std::vector<char> data;
				std::function<bool(websocket &s,std::vector<char>&)> on_data;
				websocket_packet_sink(
					std::weak_ptr<connection> c,
					int in_max_packet,
					std::function<bool(websocket &s,std::vector<char>&)> in_on_data)
				:websocket_sink(c),max_packet(in_max_packet),on_data(in_on_data) {
				}
				bool packet_start(bool fin,int type,uint64_t size) {
					if ((data.size()+size)>max_packet)
						return false;
					return true;
				}
				void packet_data(char b) {
					data.push_back(b);
				}
				bool packet_end(bool fin,int type) {
					std::shared_ptr<websocket> ws(get_websocket());
					bool ok=true;
					if (fin) {
						on_data(*ws,data);
						data.clear();
					}
					return ok;
				}
			};
			std::shared_ptr<connection> sc=std::static_pointer_cast<connection>(c.shared_from_this());
			std::shared_ptr<websocket_packet_sink> sink(
				new websocket_packet_sink(std::weak_ptr<connection>(sc),max_packet,on_data)
			);
			return make_websocket(c,sink);
		}

		response* match_file(connection &c,std::string urlprefix,std::string filepath) {
			if (0!=c.url().find(urlprefix))
				return 0; // not matching the prefix.
			std::string checked=c.url().substr(urlprefix.size());
			struct stat stbuf;
			int last='/';
			int end=checked.size();
			for (int i=0;i<checked.size();i++) {
				if (checked[i]=='\\')
					return make_text_response(500,"Bad request, \\ not allowed in url");
				if (checked[i]=='?') {
					end=i;
					break;
				}
				if (last=='/') {
					if (checked[i]=='.') {
						return 0;  // an error should be returned but could be an information leakage.
					} else if (checked[i]=='/') {
						return 0;  // an error should be returned but could be an information leakage.
					}
				}
				if (checked[i]=='/') {
					// check for directory presence!
					std::string tmp=filepath+checked.substr(0,i);
					int sr=stat( tmp.c_str(),&stbuf);
					if (sr) {
						return 0;
					}
					if (!(stbuf.st_mode&S_IFDIR)) {
						return 0;
					}
				}
				last=checked[i];
			}
			std::string tmp=filepath+checked.substr(0,end);
			if (stat(tmp.c_str(),&stbuf)) {
				return 0;
			}
			if (!(stbuf.st_mode&S_IFREG)) {
				return 0;
			}

			FILE *f=fopen(tmp.c_str(),"rb");
			if (!f)
				return 0;
			struct fh {
				FILE *f;
				fh(FILE *in_f):f(in_f){}
				~fh() {
					fclose(f);
				}
			};
			std::shared_ptr<fh> fp(new fh(f));

			return make_stream_response(200,[fp](buffer &ob) {
				int osz=ob.usage();
				int tr=ob.compact();
				int rc=fread(ob.to_produce(),1,tr,fp->f);
				if (rc>=0) {
					ob.produced(rc);
				}
				if (rc<=0) {
					//if (feof(fp->f))
					return false; // always stop sending on error
				}
				return true;
			})->set_header("content-length",std::to_string(stbuf.st_size));
		}
	}
}

