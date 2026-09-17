#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ap {
struct ObjectToken {
    uint32_t handle=0; uint64_t generation=0;
    bool operator==(const ObjectToken& o)const{return handle==o.handle&&generation==o.generation;}
};
struct Binding {
    uint32_t client=0,engine=0;int fd=-1;
    uint64_t client_generation=0;
    ObjectToken device,subdevice,group,compute_channel;
    std::vector<ObjectToken> channels,compute_objects;
    bool operator==(const Binding& o)const;
};
// Pure lifecycle model: synthetic tests use it without installing an ioctl hook.
// Historical events never participate in discover(), free(), or valid().
class ObjectRegistry {
public:
    explicit ObjectRegistry(size_t capacity=65536):capacity_(capacity){}
    void client(uint32_t handle,int held_fd);
    void allocate(uint32_t client,uint32_t handle,uint32_t parent,uint32_t cls,uint32_t engine=0);
    void bind(uint32_t client,uint32_t handle,uint32_t engine);
    void free(uint32_t client,uint32_t handle);
    void incomplete(const std::string& reason);
    Binding discover()const;
    bool valid(const Binding& b)const;
    bool contains(uint32_t client,uint32_t handle)const;
    bool has_client(uint32_t client)const;
    int client_fd(uint32_t client)const;
    std::string inventory()const;
private:
    struct Node {uint32_t client,handle,cls,engine;uint64_t generation;ObjectToken parent;};
    struct Client {int fd;uint64_t generation;};
    using Key=std::pair<uint32_t,uint32_t>;
    std::map<Key,Node> current_;
    std::map<uint32_t,Client> clients_;
    uint64_t next_=1;size_t capacity_;std::string incomplete_;
    std::vector<std::string> history_;
    const Node* node(uint32_t client,uint32_t handle)const;
    bool child_of(const Node& child,const Node& parent)const;
    void erase_tree(uint32_t client,ObjectToken root);
    void history(const std::string& event);
};
}
