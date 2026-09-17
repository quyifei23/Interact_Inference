#include "control_events.h"
#include <iostream>
int main(int argc,char** argv){
    if(argc!=3){std::cerr<<"event_dump JOURNAL.bin OUTPUT.jsonl\n";return 2;}
    try{ap::ControlJournal::recover(argv[1],argv[2]);return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
