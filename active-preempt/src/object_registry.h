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
// Independent group snapshot: there is deliberately no selected channel.
struct GroupObject {
    ObjectToken token,parent;
    uint32_t cls=0,engine=0; // zero engine means unobserved/unspecified
    bool operator==(const GroupObject& o)const;
};
struct GroupMember {
    GroupObject channel;
    std::vector<GroupObject> compute_children;
    bool operator==(const GroupMember& o)const;
};
struct GroupBinding {
    uint32_t owner_pid=0,client=0;int fd=-1;
    uint64_t registry_instance=0,client_generation=0;
    uint64_t membership_revision=0,compute_candidates_revision=0;
    std::string profile_version,source_commit;
    GroupObject device,subdevice,group;
    std::vector<GroupMember> members;
    bool operator==(const GroupBinding& o)const;
};
// Pure lifecycle model: synthetic tests use it without installing an ioctl hook.
// Historical events never participate in discover(), free(), or valid().
class ObjectRegistry {
public:
    explicit ObjectRegistry(size_t capacity=65536,std::string profile="synthetic",std::string source="synthetic");
    ObjectRegistry(const ObjectRegistry&)=delete;
    ObjectRegistry& operator=(const ObjectRegistry&)=delete;
    void client(uint32_t handle,int held_fd);
    void allocate(uint32_t client,uint32_t handle,uint32_t parent,uint32_t cls,uint32_t engine=0);
    void bind(uint32_t client,uint32_t handle,uint32_t engine);
    void free(uint32_t client,uint32_t handle);
    void incomplete(const std::string& reason);
    Binding discover()const;
    bool valid(const Binding& b)const;
    GroupBinding discover_group()const;
    bool valid_group(const GroupBinding& b)const;
    bool contains(uint32_t client,uint32_t handle)const;
    bool has_client(uint32_t client)const;
    int client_fd(uint32_t client)const;
    std::string inventory()const;
    std::string object_graph_json()const;
private:
    struct Node {uint32_t client,handle,cls,engine;uint64_t generation;ObjectToken parent;};
    struct Client {int fd;uint64_t generation;};
    using Key=std::pair<uint32_t,uint32_t>;
    std::map<Key,Node> current_;
    std::map<uint32_t,Client> clients_;
    uint64_t next_=1;size_t capacity_;std::string incomplete_;
    std::vector<std::string> history_;
    uint32_t owner_pid_;uint64_t instance_;
    std::string profile_,source_;
    std::map<Key,uint64_t> membership_revisions_;
    uint64_t next_topology_=1,compute_candidates_revision_=0;
    const Node* node(uint32_t client,uint32_t handle)const;
    bool child_of(const Node& child,const Node& parent)const;
    void erase_tree(uint32_t client,ObjectToken root);
    void history(const std::string& event);
    void touch_topology(const Node&);
};
}
