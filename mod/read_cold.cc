
#include <cassert>
#include <chrono>
#include <iostream>
#include "leveldb/db.h"
#include "leveldb/comparator.h"
#include "util.h"
#include "stats.h"
#include "learned_index.h"
#include <cstring>
#include "cxxopts.hpp"
#include <unistd.h>
#include <fstream>
#include "../db/version_set.h"
#include <cmath>
#include <random>
#include<fstream>
#include <numeric>
#include <algorithm>
#include "leveldb/write_batch.h"
#include <stdlib.h>


using namespace leveldb;
using namespace adgMod;
using namespace std;
using std::string;
using std::cout;
using std::endl;
using std::to_string;
using std::vector;
using std::map;
using std::ifstream;
using std::ofstream;
using std::string;

int num_pairs_base = 1000;
int mix_base = 20;


int buffer_size = 4*1024*1024;
int T = 10;
int num_levels = 5;
double base_bpk = 0;

leveldb::Options GetLevelingOptions() {
  leveldb::Options options;
  options.create_if_missing = true;
  options.L0_compaction_trigger = 2;
  options.write_buffer_size = buffer_size;
  options.max_bytes_for_level_base = buffer_size * T; // size for level 1, default 20MB
  options.max_bytes_for_level_multiplier = T;
  options.max_logical_level = num_levels;
  options.max_file_size = 10 * (1<<20); // 10MB
  options.base_bpk = base_bpk;
  
  leveldb::config::kL0_CompactionTrigger = 2;
  leveldb::config::kL0_SlowdownWritesTrigger = 2;
  leveldb::config::kL0_StopWritesTrigger = 4;

  return options;
}

leveldb::Options GetTieringOptions() {
  leveldb::Options options;
  options.create_if_missing = true;
  options.L0_compaction_trigger = T;
  options.write_buffer_size = buffer_size;
  options.max_bytes_for_level_base = buffer_size * T * T; // size for level 1, default 200MB
  options.max_bytes_for_level_multiplier = T;
  options.max_logical_level = num_levels;
  options.max_file_size = 10 * (1<<20); // 10MB
  options.compaction_policy = leveldb::kCompactionStyleTier;
  options.base_bpk = base_bpk;

  leveldb::config::kL0_CompactionTrigger = T;
  leveldb::config::kL0_SlowdownWritesTrigger = T + 2;
  leveldb::config::kL0_StopWritesTrigger = T + 4;
  
  return options;
}

leveldb::Options GetLazyLevelOptions() {
  leveldb::Options options;
  options.create_if_missing = true;
  options.L0_compaction_trigger = T;
  options.write_buffer_size = buffer_size;
  options.max_bytes_for_level_base = buffer_size * T * T; // size for level 1, default 200MB
  options.max_bytes_for_level_multiplier = T;
  options.max_logical_level = num_levels;
  options.max_file_size = 10 * (1<<20); // 10MB
  options.compaction_policy = leveldb::kCompactionStyleLazyLevel;
  options.base_bpk = base_bpk;

  leveldb::config::kL0_CompactionTrigger = T;
  leveldb::config::kL0_SlowdownWritesTrigger = T + 2;
  leveldb::config::kL0_StopWritesTrigger = T + 4;
  
  return options;
}
class NumericalComparator : public Comparator {
public:
    NumericalComparator() = default;
    virtual const char* Name() const {return "adgMod:NumericalComparator";}
    virtual int Compare(const Slice& a, const Slice& b) const {
        uint64_t ia = adgMod::ExtractInteger(a.data(), a.size());
        uint64_t ib = adgMod::ExtractInteger(b.data(), b.size());
        if (ia < ib) return -1;
        else if (ia == ib) return 0;
        else return 1;
    }
    virtual void FindShortestSeparator(std::string* start, const Slice& limit) const { return; };
    virtual void FindShortSuccessor(std::string* key) const { return; };
};


void PutAndPrefetch(int lower, int higher, vector<string>& keys) {
    adgMod::Stats* instance = adgMod::Stats::GetInstance();

    Status status;

    instance->StartTimer(9);
    for (int i = lower; i < higher; ++i) {
        string value = generate_value(0);
        status = db->Put(write_options, keys[i], value);
        assert(status.ok() && "File Put Error");
    }
    instance->PauseTimer(9, true);

    cout << "Put Complete in prefetch()" << endl;


    instance->StartTimer(10);
    for (int i = lower; i < higher; ++i) {
        string value;
        status = db->Get(read_options, keys[i], &value);
        cout << "Get " << keys[i] << " Done" << endl;
        assert(status.ok() && "File Get Error");
    }
    instance->PauseTimer(10, true);

    cout << "Prefetch Complete" << endl;
};

enum LoadType {
    Ordered = 0,
    Reversed = 1,
    ReversedChunk = 2,
    Random = 3,
    RandomChunk = 4
};

int main(int argc, char *argv[]) {
    int rc;
    int num_operations, num_iteration, num_mix, write_buffer_size;
    size_t max_file_size, block_size;
    float test_num_segments_base;
    float num_pair_step;
    string db_location, profiler_out, input_filename, distribution_filename, ycsb_filename, read_filename;
    bool print_single_timing, print_file_info, evict, unlimit_fd, use_distribution = false, pause, use_ycsb = false;
    bool change_level_load, change_file_load, change_level_learning, change_file_learning;
    int load_type, insert_bound, length_range;
    string db_location_copy;
    double bpk, model_error;
    int dataset_no, workload_no, exp_no;
    string dataset_name;
    int compaction_policy,inputlen;

    //0:plr, 1:lipp 2:ft 3.pgm 4.rmi 5.rs 6. plex 7.dili 8. alex
    map<int, string> ModelMap = {
        {0, "Piece-Wise"},
        {1, "LIPP"},
        {2, "FITing-Tree"},
        {3, "PGM"},
        {4, "RMI"},
        {5, "RS"},
        {6, "PLEX"},
        {7, "DILI"},
        {8, "ALEX"}
    };

    map<int, string> DatasetMap = {
        {0, "Random"},
        {1, "lognormal"},
        {2, "normal"},
        {3, "segment"},
        {4, "longitude"},
        {5, "longlat"},
        {6, "books"},
        {7, "fb"},
        {8, "osm_cellid"},
        {9, "wiki"},
    };

    map<int, string> WorkloadMap = {
        {0, "Uniform"},
        {1, "Zipfian"},
        {2, "Latest"},
        {3, "YCSB-A"},
        {4, "YCSB-B"},
        {5, "YCSB-C"},
        {6, "YCSB-D"},
    };

    map<int, string> ExpMap = {
        {0, "PointQuery"},
        {1, "Distributions"},
        {2, "Range Queries"},
        {3, "YCSB"},
        {4, "SOSD"},
    };

    string output;
    cout.setf(std::ios::fixed,std::ios::floatfield);


    cxxopts::Options commandline_options("leveldb read test", "Testing leveldb read performance.");
    commandline_options.add_options()
            ("n,get_number", "the number of gets (to be multiplied by 1024)", cxxopts::value<int>(num_operations)->default_value("1000"))
            ("s,step", "the step of the loop of the size of db", cxxopts::value<float>(num_pair_step)->default_value("1"))
            ("i,iteration", "the number of iterations of a same size", cxxopts::value<int>(num_iteration)->default_value("1"))
            ("m,modification", "if set, run our modified version, 7 file-model bourbon, 8 wiskey, 9 ours", cxxopts::value<int>(adgMod::MOD)->default_value("0"))
            ("h,help", "print help message", cxxopts::value<bool>()->default_value("false"))
            ("d,directory", "the directory of db", cxxopts::value<string>(db_location)->default_value("/mnt/ssd/testdb"))
            ("k,key_size", "the size of key", cxxopts::value<int>(adgMod::key_size)->default_value("16"))
            ("v,value_size", "the size of value", cxxopts::value<int>(adgMod::value_size)->default_value("8"))
            ("single_timing", "print the time of every single get", cxxopts::value<bool>(print_single_timing)->default_value("false"))
            ("file_info", "print the file structure info", cxxopts::value<bool>(print_file_info)->default_value("false"))
            ("test_num_segments", "test: number of segments per level", cxxopts::value<float>(test_num_segments_base)->default_value("1"))
            ("string_mode", "test: use string or int in model", cxxopts::value<bool>(adgMod::string_mode)->default_value("false"))
            ("file_model_error", "error in file model", cxxopts::value<uint32_t>(adgMod::file_model_error)->default_value("8"))
            ("level_model_error", "error in level model", cxxopts::value<uint32_t>(adgMod::level_model_error)->default_value("1"))
            ("f,input_file", "the filename of input file", cxxopts::value<string>(input_filename)->default_value(""))
            ("blocksize", "block size in SSTables", cxxopts::value<size_t>(block_size)->default_value("4096"))
            ("SST", "SST size", cxxopts::value<size_t>(max_file_size)->default_value("4194304"))
            ("buffersize","db write buffer size", cxxopts::value<int>(write_buffer_size)->default_value("4194304"))
            ("r,read_file", "the filename of read file", cxxopts::value<string>(read_filename)->default_value(""))
            ("multiple", "test: use larger keys", cxxopts::value<uint64_t>(adgMod::key_multiple)->default_value("1"))
            ("w,write", "writedb", cxxopts::value<bool>(fresh_write)->default_value("false"))
            ("c,uncache", "evict cache", cxxopts::value<bool>(evict)->default_value("false"))
            ("u,unlimit_fd", "unlimit fd", cxxopts::value<bool>(unlimit_fd)->default_value("false"))
            ("b,bpk", "bits per key", cxxopts::value<double>(bpk)->default_value("10"))
            ("e,modelerror", "bits per key", cxxopts::value<double>(model_error)->default_value("8"))
            ("x,dummy", "dummy option")
            ("l,load_type", "load type", cxxopts::value<int>(load_type)->default_value("0"))
            // ("filter", "use filter", cxxopts::value<bool>(adgMod::use_filter)->default_value("false"))
            ("mix", "portion of writes in workload in 1000 operations", cxxopts::value<int>(num_mix)->default_value("0"))
            ("distribution", "operation distribution", cxxopts::value<string>(distribution_filename)->default_value(""))
            ("change_level_load", "load level model", cxxopts::value<bool>(change_level_load)->default_value("false"))
            ("change_file_load", "enable level learning", cxxopts::value<bool>(change_file_load)->default_value("false"))
            ("p,pause", "pause between operation", cxxopts::value<bool>(pause)->default_value("false"))
            ("policy", "learn policy", cxxopts::value<int>(adgMod::policy)->default_value("0"))
            ("YCSB", "use YCSB trace", cxxopts::value<string>(ycsb_filename)->default_value(""))
            ("insert", "insert new value", cxxopts::value<int>(insert_bound)->default_value("0"))
            ("modelmode", "mm=0 plr, mm=1 lipp", cxxopts::value<int>(adgMod::modelmode)->default_value("0"))
            ("RSbits","RS radix bits",cxxopts::value<int>(adgMod::RSbits)->default_value("18"))
            ("rmisize","RMI layer2 size",cxxopts::value<int>(adgMod::rmi_layer_size)->default_value("1024"))
            ("lippgap","lipp gap", cxxopts::value<int>(adgMod::lipp_gap)->default_value("5"))
            ("alexnodesize","alex max node size",cxxopts::value<int>(adgMod::alex_node_size)->default_value("2048"))
            ("alexinterval", "alex*", cxxopts::value<int>(adgMod::alex_interval)->default_value("1"))
            ("lippmode", "lipp*", cxxopts::value<int>(adgMod::lipp_mode)->default_value("0"))
            ("dataset","name of dataset", cxxopts::value<string>(dataset_name)->default_value("Unknown"))
            // ("dataset","name of dataset", cxxopts::value<int>(dataset_no)->default_value("0"))
            ("inputlen","if >0, limits the number of input keys and disables reading", cxxopts::value<int>(inputlen)->default_value("0"))
            ("workload","name of workload", cxxopts::value<int>(workload_no)->default_value("0"))
            ("exp","name of workload", cxxopts::value<int>(exp_no)->default_value("0"))
            ("range", "use range query and specify length", cxxopts::value<int>(length_range)->default_value("0"))
            ("output", "output key list", cxxopts::value<string>(output)->default_value("key_list.txt"))
            ("monkey", "monkey bpk of level 0, set to 0 to disable", cxxopts::value<double>(base_bpk)->default_value("0"))
            ("compactionpolicy", "LevelDB Compaction Policy", cxxopts::value<int>(compaction_policy)->default_value("0"));
    auto result = commandline_options.parse(argc, argv);
    if (result.count("help")) {
        printf("%s", commandline_options.help().c_str());
        exit(0);
    }

    if(db_location.back()!='/')
        db_location+='/';

    std::default_random_engine e1(0), e2(255), e3(0);
    srand(0);
    num_operations *= num_pairs_base;
    db_location_copy = db_location;

    adgMod::fd_limit = unlimit_fd ? 1024 * 1024 : 1024;
    adgMod::restart_read = true;
    adgMod::load_level_model = false;
    adgMod::load_file_model ^= change_file_load;
    adgMod::file_model_error = model_error;
    adgMod::level_model_error = model_error;
    adgMod::db_name = db_location;
    adgMod::rangelength = length_range;

    cout<<"load level model: "<<adgMod::load_level_model<<endl;

    cout<<"load file model: "<<adgMod::load_file_model<<endl;


    vector<string> keys;
    vector<pair<string,string>> keys_read;
    vector<uint64_t> distribution;
    vector<int> ycsb_is_write;
    //keys.reserve(100000000000 / adgMod::value_size);
    if (!input_filename.empty()) {
        cout<<"input file: "<<input_filename<<endl;
        ifstream input(input_filename);
        uint64_t key;
        string op;
        while (input >>op>> key) {
        // while (input >> key) {
            // std::cout<<key<<std::endl;
            
            op = "INSERT";
            
            string the_key = generate_key(std::to_string(key));
            // std::cout<<the_key<<std::endl;
            keys.push_back(std::move(the_key));
            // break;
            if(keys.size()==inputlen)
                break;
        }
        //adgMod::key_size = (int) keys.front().size();
    } else {
        std::uniform_int_distribution<uint64_t> udist_key(0, 999999999999999);
        for (int i = 0; i < 1000000; ++i) {
            keys.push_back(generate_key(to_string(udist_key(e2))));
        }
    }

    if (!read_filename.empty()) {
        ifstream inputr(read_filename);
        string key;
        string op;
        while (inputr >> op >> key) {
            // cout<<op<<key<<endl;
            string the_key = generate_key(key);
            keys_read.push_back(make_pair(std::move(op),std::move(the_key)));
        }
        //adgMod::key_size = (int) keys.front().size();
    } else {
        std::uniform_int_distribution<uint64_t> udist_key(0, 999999999999999);
        for (int i = 0; i < 1000000; ++i) {
            keys_read.push_back(make_pair("GET", generate_key(to_string(udist_key(e2)))));
        }
    }

    if (!distribution_filename.empty()) {
        use_distribution = true;
        ifstream input(distribution_filename);
        uint64_t index;
        while (input >> index) {
            distribution.push_back(index);
        }
    }

    if (!ycsb_filename.empty()) {
        use_ycsb = true;
        use_distribution = true;
        ifstream input(ycsb_filename);
        uint64_t index;
        int is_write;
        while (input >> is_write >> index) {
            distribution.push_back(index);
            ycsb_is_write.push_back(is_write);
        }
    }
    bool copy_out = num_mix != 0 || use_ycsb;

    adgMod::Stats* instance = adgMod::Stats::GetInstance();
    vector<vector<size_t>> times(20);
    string values(1024 * 1024, '0');

    cout<<"keys list size is: "<<keys.size()<<endl;
    cout<<"each key size is: "<<keys[0].size()<<endl;
    cout<<"read keys list size is: "<<keys_read.size()<<endl;
    cout<<"each read key size is: "<<keys_read[0].second.size()<<endl;

    cout<<"No fence parameter is: "<<adgMod::nofence<<endl;

    if (copy_out) {
        rc = system("sync; echo 3 | sudo tee /proc/sys/vm/drop_caches");
    }

    if (num_mix > 1000) {
        mix_base = 1000;
        num_mix -= 1000;
    }

    for (size_t iteration = 0; iteration < num_iteration; ++iteration) {
//        if (copy_out) {
//            rc = system("sudo fstrim -a -v");
//        }

        db_location = db_location_copy;
        std::uniform_int_distribution<uint64_t > uniform_dist_file(0, (uint64_t) keys.size() - 1);
        std::uniform_int_distribution<uint64_t > uniform_dist_file2(0, (uint64_t) keys.size() - 1);
        std::uniform_int_distribution<uint64_t > uniform_dist_value(0, (uint64_t) values.size() - adgMod::value_size - 1);
        clock_t  ReadBegin, ReadEnd, WirteBegin, WriteEnd, TrainBegin, TrainEnd;
        double ReadDuration, WriteDuration, TrainDuration;


        DB* db;
        Options options;
        ReadOptions& read_options = adgMod::read_options;
        WriteOptions& write_options = adgMod::write_options;
        Status status;

        buffer_size = write_buffer_size;
        if(compaction_policy==0) // leveling
        {
            options=GetLevelingOptions();
            options.write_buffer_size = write_buffer_size;
            options.max_bytes_for_level_base = write_buffer_size * T;
        }
        else if (compaction_policy == 1) // tiering
        {
            options = GetTieringOptions();
        }
        else if (compaction_policy == 2) //lazy leveling
        {
            options = GetLazyLevelOptions();
        }
        options.create_if_missing = true;
        options.filter_policy = leveldb::NewBloomFilterPolicy(bpk);
        options.max_file_size=max_file_size;
        // if(options.max_file_size<block_size)
        //     options.max_file_size=block_size;
        options.block_size = block_size;
        options.write_buffer_size=write_buffer_size;
        cout<<"Block size: "<<options.block_size<<endl;
        cout<<"Table size: "<<options.max_file_size<<endl;
        cout<<"Write buffer size: "<<options.write_buffer_size<<endl;
        // options.block_size = options.max_file_size;
        //options.comparator = new NumericalComparator;
        //adgMod::block_restart_interval = options.block_restart_interval = adgMod::MOD == 8 || adgMod::MOD == 7 ? 1 : adgMod::block_restart_interval;
        read_options.fill_cache = false;
        write_options.sync = false;
        instance->ResetAll();


        if (fresh_write && iteration == 0) {
//             // Load DB
            // clear existing directory, clear page cache, trim SSD
            string command = "rm -rf " + db_location;
            rc = system(command.c_str());
        //    rc = system("sudo fstrim -a -v");
        //    rc = system("sync; echo 3 | sudo tee /proc/sys/vm/drop_caches");
           cout << "delete and trim complete" << endl;
            // string delete_models = "find " + db_location + " -type f -name \"*.fmodel\" -delete";
            // std::cout<<delete_models<<std::endl;
            // rc = system(delete_models.c_str());

            cout << "Opening DB" << endl;
            status = DB::Open(options, db_location, &db);
            assert(status.ok() && "Open Error");
            cout << "Opened DB" << endl;

            // leveldb::WriteBatch batch;
            // for(int i = 0; i<keys.size(); i++){
            //     batch.Put(keys[i], {values.data() + uniform_dist_value(e2), (uint64_t) adgMod::value_size});
            // }
            // std::cout<<"finished batch load"<<std::endl;
            // // instance->StartTimer(9);
            // status = db->Write(write_options, &batch);
            // perform loadi
            for (int i = 0; i < keys.size(); ++i) {
                if(i%(keys.size()/10)==0)
                {
                    cout<<i/(keys.size()/10)<<"/10"<<endl;
                }
                // cout << keys[i] << endl;
                status = db->Put(write_options, keys[i], {values.data() + uniform_dist_value(e2), (uint64_t) adgMod::value_size});
                assert(status.ok() && "File Put Error");
                if(!status.ok()){
                    cout<<status.ToString()<<endl;
                }
                // break;
            }
            cout << "Put Complete ready to sync" << endl;
            adgMod::db->vlog->Sync();
            cout << "Put Complete" << endl;

            ofstream fd;
            fd.open(db_location + "/" + output);
            for (int i = 0; i < keys.size(); ++i)
                fd << keys[i] << "\n";
            fd.close();

            keys.clear();
            
            sleep(1);
            adgMod::db->WaitForBackground();
            // reopen DB and do offline leraning
            if (print_file_info && iteration == 0) db->PrintFileInfo();
            string expname = ExpMap[exp_no];
            std::fstream res;
            res.setf(std::ios::fixed,std::ios::floatfield);
            res.open("/home/jiarui/LearnedIndexInLSM/evaluation/" + expname + ".out",std::ios::out|std::ios::app);
            res << "bloom filter size: " << bloom_size << endl;
            cout << "bloom filter size: " << bloom_size << endl;
            res.close();
            delete db;
            if(inputlen>0)
                return 0;

        
            status = DB::Open(options, db_location, &db);
            adgMod::db->WaitForBackground();
            cout << "Repoened" << endl;
            auto TrainTimeStart = std::chrono::steady_clock::now();
            if (adgMod::MOD == 6 || adgMod::MOD == 7 || adgMod::MOD == 9) {
                Version* current = adgMod::db->versions_->current();

                // offline level learning
                // for (int i = 1; i < config::kNumLevels; ++i) {
                //     LearnedIndexData::LevelLearn(new VersionAndSelf{current, adgMod::db->version_count, current->learned_index_data_[i].get(), i});
                // }

                // offline file learning
                cout << "File Learning..." << endl;
                current->FileLearn();
                cout << "File Learn complete..." << endl;
            }
            auto TrainTimeEnd = std::chrono::steady_clock::now();
            double TrainTimeMicro = std::chrono::duration<double, std::micro>(TrainTimeEnd - TrainTimeStart).count();
            // file_data->Report();
            // adgMod::learn_cb_model->Report();
            cout << "Shutting down" << endl;
            adgMod::db->WaitForBackground();
            delete db;

            status = DB::Open(options, db_location, &db);
            assert(status.ok() && "Open Error");
            adgMod::db->WaitForBackground();


            cout << "Repoened" << endl;
            vector<double> results;

            
            fresh_write=false;
            for(int j=0; j<1; j++){
                // cout<<"In multi read"<<endl;
                int hit_times = 0;
                int get_times = 0;
                // adgMod::db->WaitForBackground();
                adgMod::db->WaitForBackground();
                delete db;
                status = DB::Open(options, db_location, &db);
                adgMod::db->WaitForBackground();

                cout << "Ready to read" << endl;
                auto ReadTimeStart = std::chrono::steady_clock::now();
                string value;
                for(int i=0; i<keys_read.size(); i++){
                // for(int i=0; i<500; i++){
                    if(i%100000 == 0){
                        cout<<i<<" "<<keys_read[i].first<<" "<<keys_read[i].second<<endl;
                    }
                    
                    string option = keys_read[i].first;
                    if(option == "READ"){
                        // cout<<"Reading!"<<endl;
                        get_times++;
                        status = db->Get(read_options, keys_read[i].second, &value);
                    if (status.ok()) {
                            hit_times++;
                            // cout<<"Non zero!"<<endl;
                        }
                        if (!status.ok()) {
                            cout << keys_read[i].second << " Not Found" << endl;
                            assert(status.ok() && "File Get Error");
                            return -1;
                        }
                    }
                    else if(option == "INSERT"){
                        status = db->Put(write_options, keys_read[i].second, {values.data() + uniform_dist_value(e2), (uint64_t) adgMod::value_size});
                    }
                    else if(option == "SCAN"){
                        // std::cout<<"operation "<<i<<" doing scan"<<std::endl;
                        Iterator* db_iter = length_range == 0 ? nullptr : db->NewIterator(read_options);
                        db_iter->Seek(keys_read[i].second);
                        // Range
                        for (int r = 0; r < length_range; ++r) {
                            if (!db_iter->Valid()) break;
                            Slice key = db_iter->key();
                            string value = db_iter->value().ToString();
                            // cout << key.ToString() << value << endl;
                            // value.clear();
                            db_iter->Next();
                        }
                        delete db_iter;
                    }
                }
                auto ReadTimeEnd = std::chrono::steady_clock::now();
                double ReadTimeMicro = std::chrono::duration<double, std::micro>(ReadTimeEnd - ReadTimeStart).count();
                ReadDuration = ReadTimeMicro;
                results.push_back(ReadDuration);
                cout<<"Hit times: "<<hit_times<<" Get times: "<<get_times<<endl;
            }
            adgMod::db->WaitForBackground();
            double ReadTimesum = accumulate(begin(results), end(results), 0.0);  
            double ReadTimeMean =  ReadTimesum / results.size();
            cout<<"Read complete"<<endl;

            Version* current = adgMod::db->versions_->current();
            adgMod::MOD = MOD;
            int FencePointersize = ((options.max_file_size / options.block_size) +1) * key_size /2*3;
            current = adgMod::db->versions_->current();
            int file_num = current->GetFileNum();
            FencePointersize *= file_num; 
            cout<<"Fence Pointer size is "<< FencePointersize <<" Byte."<<endl;


            long long int size_byte = adgMod::file_data->Getmodelsize();
            cout<<"Learned/LIPP index size is "<< size_byte <<" Byte."<<endl;
            delete db;
            // Managing outputs

            
            
            string modelname = ModelMap[adgMod::modelmode];
            if(modelmode==5)
                modelname=modelname+"-"+std::to_string(RSbits);
            // else if(modelmode==3)
            //     modelname=modelname+"-"+std::to_string(PGM_internal_Error);
            else if(modelmode==1)
                modelname=modelname+"-"+std::to_string(lipp_gap);
            else if(modelmode==8)
                modelname=modelname+"-"+std::to_string(alex_node_size);
            string exptag = modelname + "_" + dataset_name + "_" + std::to_string(length_range)+"_"+std::to_string(model_error);
            cout << "bloom filter size: " << bloom_size << " fence pointer size: "<< FencePointersize << " index size: "<< size_byte << endl;
            cout << exptag << "\t" << filelearn_count << "/" << newSST_count
                 << " " << learn_duration << " " << compaction_duration << "/"
                 << compaction_count << "\t" << compaction_size_outputs << "/"
                 << compaction_size_inputs << endl;
            res << "bloom filter size: " << bloom_size << " fence pointer size: "<< FencePointersize << " index size: "<< size_byte << endl;
            res << exptag << "\t" << filelearn_count << "/" << newSST_count
                << " " << learn_duration << "/" << compaction_duration << "\t"
                << compaction_count << "\t" << compaction_size_outputs << "/"
                << compaction_size_inputs << endl;
            res.close();
            
        }
    }
}

