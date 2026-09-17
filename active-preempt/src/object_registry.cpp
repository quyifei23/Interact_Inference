#include "object_registry.h"
#include <sstream>
#include <stdexcept>

namespace ap {
namespace {
bool channel(uint32_t c){return c==0xc36f||c==0xc46f||c==0xc56f||c==0xc86f;}
bool compute(uint32_t c){return c==0xc5c0||c==0xc6c0||c==0xc7c0;}
[[noreturn]] void unavailable(const std::string& s){throw std::runtime_error("OBJECT_BINDING_UNAVAILABLE: "+s);}
}
bool Binding::operator==(const Binding& o)const {
    return client==o.client&&client_generation==o.client_generation&&fd==o.fd&&engine==o.engine&&
        device==o.device&&subdevice==o.subdevice&&group==o.group&&compute_channel==o.compute_channel&&
        channels==o.channels&&compute_objects==o.compute_objects;
}
void ObjectRegistry::history(const std::string& event){
    // Bounded diagnostics. This cap is not a guessed channel count. If exceeded,
    // capture is explicitly incomplete and no control may use the registry.
    if(history_.size()>=capacity_*4){incomplete("event history capacity exceeded");return;}
    history_.push_back(event);
}
void ObjectRegistry::incomplete(const std::string& reason){if(incomplete_.empty())incomplete_=reason;}
bool ObjectRegistry::has_client(uint32_t c)const{return clients_.count(c)!=0;}
int ObjectRegistry::client_fd(uint32_t c)const{auto i=clients_.find(c);return i==clients_.end()?-1:i->second.fd;}
void ObjectRegistry::client(uint32_t c,int fd){
    if(!c||fd<0){incomplete("missing owned control FD/client");return;}
    if(has_client(c))return;
    if(clients_.size()>=capacity_){incomplete("client capacity exceeded");return;}
    clients_[c]={fd,next_++};history("client "+std::to_string(c));
}
const ObjectRegistry::Node* ObjectRegistry::node(uint32_t c,uint32_t h)const{
    auto i=current_.find({c,h});return i==current_.end()?nullptr:&i->second;
}
bool ObjectRegistry::contains(uint32_t c,uint32_t h)const{return node(c,h)!=nullptr;}
bool ObjectRegistry::child_of(const Node& c,const Node& p)const{return c.client==p.client&&c.parent==ObjectToken{p.handle,p.generation};}
void ObjectRegistry::erase_tree(uint32_t c,ObjectToken root){
    std::vector<ObjectToken> doomed{root};
    for(size_t i=0;i<doomed.size();++i)
        for(const auto& kv:current_)if(kv.second.client==c&&kv.second.parent==doomed[i])
            doomed.push_back({kv.second.handle,kv.second.generation});
    for(const auto& t:doomed){auto i=current_.find({c,t.handle});if(i!=current_.end()&&i->second.generation==t.generation)current_.erase(i);}
}
void ObjectRegistry::allocate(uint32_t c,uint32_t h,uint32_t p,uint32_t cls,uint32_t engine){
    if(!has_client(c)){incomplete("allocation without observed client FD");return;}
    if(const auto* old=node(c,h))erase_tree(c,{h,old->generation});
    if(current_.size()>=capacity_){incomplete("object capacity exceeded");return;}
    const auto* parent=node(c,p);ObjectToken token{p,parent?parent->generation:0};
    Node n{c,h,cls,engine,next_++,token};current_[{c,h}]=n;
    history("alloc client="+std::to_string(c)+" handle="+std::to_string(h)+" generation="+std::to_string(n.generation));
}
void ObjectRegistry::bind(uint32_t c,uint32_t h,uint32_t engine){
    auto i=current_.find({c,h});if(i==current_.end()){incomplete("bind to unobserved object");return;}
    // Changing an engine invalidates snapshots even if later rebound to its old
    // value. Preserve children while advancing this parent's generation.
    auto old=i->second.generation;i->second.generation=next_++;i->second.engine=engine;
    for(auto& kv:current_)if(kv.second.client==c&&kv.second.parent==ObjectToken{h,old})kv.second.parent.generation=i->second.generation;
    history("bind "+std::to_string(h));
}
void ObjectRegistry::free(uint32_t c,uint32_t h){
    if(c==h){
        for(auto i=current_.begin();i!=current_.end();)if(i->first.first==c)i=current_.erase(i);else ++i;
        clients_.erase(c);
    }else if(const auto* n=node(c,h))erase_tree(c,{h,n->generation});
    history("free client="+std::to_string(c)+" handle="+std::to_string(h));
}
Binding ObjectRegistry::discover()const{
    if(!incomplete_.empty())unavailable("incomplete registry: "+incomplete_);
    Binding result;unsigned groups=0;
    for(const auto& kv:current_){const auto& g=kv.second;if(g.cls!=0xa06c)continue;
        Binding b;b.client=g.client;b.group={g.handle,g.generation};b.engine=g.engine;
        unsigned compute_channels=0;
        for(const auto& ck:current_){const auto& ch=ck.second;if(!channel(ch.cls)||!child_of(ch,g))continue;
            b.channels.push_back({ch.handle,ch.generation});bool is_compute=false;
            for(const auto& ek:current_){const auto& e=ek.second;if(compute(e.cls)&&child_of(e,ch)){is_compute=true;b.compute_objects.push_back({e.handle,e.generation});}}
            if(is_compute){++compute_channels;b.compute_channel={ch.handle,ch.generation};if(!b.engine)b.engine=ch.engine;}
        }
        if(!compute_channels)continue;
        if(compute_channels!=1)unavailable("multiple compute channels in one TSG");
        const auto* dev=node(g.client,g.parent.handle);
        if(!dev||dev->cls!=0x80||!child_of(g,*dev))unavailable("device ancestry missing");
        b.device={dev->handle,dev->generation};unsigned subs=0;
        for(const auto& sk:current_)if(sk.second.cls==0x2080&&child_of(sk.second,*dev)){
            b.subdevice={sk.second.handle,sk.second.generation};++subs;
        }
        auto ci=clients_.find(g.client);
        if(subs!=1||ci==clients_.end())unavailable("no unique subdevice/owned client FD");
        b.fd=ci->second.fd;b.client_generation=ci->second.generation;result=b;++groups;
    }
    if(groups!=1)unavailable("expected exactly one owned compute TSG; found "+std::to_string(groups));
    return result;
}
bool ObjectRegistry::valid(const Binding& b)const{
    if(!incomplete_.empty())return false;
    auto ci=clients_.find(b.client);
    if(ci==clients_.end()||ci->second.generation!=b.client_generation||ci->second.fd!=b.fd)return false;
    for(auto t:{b.device,b.subdevice,b.group,b.compute_channel}){
        const auto* n=node(b.client,t.handle);if(!n||n->generation!=t.generation)return false;
    }
    size_t channels=0,objects=0,subdevices=0;
    const auto* group=node(b.client,b.group.handle);
    const auto* device=node(b.client,b.device.handle);
    for(const auto& kv:current_){const auto& n=kv.second;
        if(n.cls==0x2080&&child_of(n,*device))++subdevices;
        if(channel(n.cls)&&child_of(n,*group)){
            if(channels>=b.channels.size()||!(b.channels[channels++]==ObjectToken{n.handle,n.generation}))return false;
        }
        if(compute(n.cls)){
            const auto* ch=node(n.client,n.parent.handle);
            if(!ch||!channel(ch->cls)||!child_of(n,*ch))continue;
            const auto* g=node(ch->client,ch->parent.handle);
            if(!g||g->cls!=0xa06c||!child_of(*ch,*g))continue;
            if(n.client!=b.client||g->generation!=b.group.generation||ch->generation!=b.compute_channel.generation)return false;
            if(objects>=b.compute_objects.size()||!(b.compute_objects[objects++]==ObjectToken{n.handle,n.generation}))return false;
        }
    }
    return subdevices==1&&channels==b.channels.size()&&objects==b.compute_objects.size();
}
std::string ObjectRegistry::inventory()const{
    std::ostringstream s;s<<"incomplete="<<(incomplete_.empty()?"no":incomplete_)<<" current_objects="<<current_.size()<<"\n";
    for(const auto& kv:current_){const auto& n=kv.second;s<<"client="<<n.client<<" handle="<<n.handle<<" class="<<std::hex<<n.cls<<std::dec<<" generation="<<n.generation<<" parent="<<n.parent.handle<<" parent_generation="<<n.parent.generation<<" engine="<<n.engine<<"\n";}
    s<<"history (not used for liveness):\n";for(const auto& e:history_)s<<e<<'\n';return s.str();
}
}
