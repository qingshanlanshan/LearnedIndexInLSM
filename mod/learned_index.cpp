//
// Created by daiyi on 2020/02/02.
//

#include "learned_index.h"

#include "db/version_set.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <utility>
#include <iostream>
#include <string>
#include "mod/pgm/pgm_index.hpp"
#include "util/mutexlock.h"
// #include <mod/lipp/core/storage_management.h>
#include "mod/PLEX/include/ts/ts.h"
#include "mod/PLEX/include/ts/builder.h"
#include "mod/PLEX/include/ts/serializer.h"
#include "mod/DILI/src/dili/DILI.h"
#include "util.h"

long BLOCK_SIZE = 8192/2;

namespace adgMod {

std::pair<uint64_t, uint64_t> LearnedIndexData::GetPosition(
    const Slice& target_x) const {
  assert(string_segments.size() > 1);
  ++served;
  // check if the key is within the model bounds
  uint64_t target_int = SliceToInteger(target_x);
  if (target_int > max_key) return std::make_pair(size, size);
  if (target_int < min_key) return std::make_pair(size, size);

  // binary search between segments
  uint32_t left = 0, right = (uint32_t)string_segments.size() - 1;
  while (left != right - 1) {
    uint32_t mid = (right + left) / 2;
    if (target_int < string_segments[mid].x)
      right = mid;
    else
      left = mid;
  }


  // calculate the interval according to the selected segment
  uint64_t lower, upper;
  if(adgMod::nofence == 1){
    double result = target_int * string_segments[left].k + string_segments[left].b;
    lower = result;
    upper = lower;
    if(is_level){
      double result = target_int * string_segments[left].k + string_segments[left].b;
      result = is_level ? result / 2 : result;
      lower =
          result - error > 0 ? (uint64_t)std::floor(result - error) : 0;
      upper = (uint64_t)std::ceil(result + error);
      if (lower >= size) return std::make_pair(size, size);
      upper = upper < size ? upper : size - 1;
    }
  }
  else if((adgMod::nofence == 2)){
    double result = target_int * string_segments[left].k + string_segments[left].b;
      result = is_level ? result / 2 : result;
      lower =
          result -  adgMod::block_num_entries - error > 0 ? (uint64_t)std::floor(result - adgMod::block_num_entries - error) : 0;
      upper = (uint64_t)std::ceil(result + adgMod::block_num_entries + error);
      if (lower >= size) return std::make_pair(size, size);
      upper = upper < size ? upper : size - 1;
  }
  else{
      double result = target_int * string_segments[left].k + string_segments[left].b;
      result = is_level ? result / 2 : result;
      lower =
          result - error > 0 ? (uint64_t)std::floor(result - error) : 0;
      upper = (uint64_t)std::ceil(result + error);
      if (lower >= size) return std::make_pair(size, size);
      upper = upper < size ? upper : size - 1;
  }

  return std::make_pair(lower, upper);
}

uint64_t LearnedIndexData::MaxPosition() const { return size - 1; }

double LearnedIndexData::GetError() const { return error; }

// Actual function doing learning
bool LearnedIndexData::Learn(string filename) {
  // FILL IN GAMMA (error)

  int count = string_keys.size();
  real_num_entries = count;
  string number_file_name = adgMod::db_name + "/" + filename + ".fnum";

  std::ofstream outfile;
  outfile.open(number_file_name);
  if (outfile.is_open())
  {
      outfile << count;
      outfile.close();
  }
  if(count<10){
    learned.store(false);
    return false;
  } 
  // else
  // {
  //     std::cout << "Unable to open file";
  // }


  if(adgMod::modelmode == 0){
    PLR plr = PLR(error);

    // check if data if filled
    if (string_keys.empty()) assert(false);

    // fill in some bounds for the model
    uint64_t temp = atoll(string_keys.back().first.c_str());
    min_key = atoll(string_keys.front().first.c_str());
    max_key = atoll(string_keys.back().first.c_str());
    size = string_keys.size();


    // actual training
    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key == thiskey){
          string_keys[i].first = std::to_string(last_key +1);
        }
        last_key = stoll(string_keys[i].first);
    }


    std::vector<Segment> segs = plr.train(string_keys, !is_level);
    if (segs.empty()) return false;
    // fill in a dummy last segment (used in segment binary search)
    segs.push_back((Segment){temp, 0, 0});
    string_segments = std::move(segs);

    learned.store(true);
  }
  else if(adgMod::modelmode == 1){
    // std::cout<<"LIPP learning"<<std::endl;
    string index_name = adgMod::db_name + "/" + filename + ".fmodel";
    const char* index_name_p = index_name.c_str();
    lipp_index.init(index_name_p, true);

    lippKeyType *keys = new lippKeyType[count];
    lippValueType *values = new lippValueType[count];
    // std::cout<<"key count"<<count<<std::endl;

    // 57 57 57 57
    // 57 58 57
    //last key 58
    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key >= thiskey){
          keys[i]= last_key +1;
          // cout<<"turns "<<last_key<<" to "<<keys[i]<<endl;
        }
        else{
          keys[i] = thiskey;
        }
        values[i] = i;
        last_key = keys[i];
    }

    for(int i=0; i<count; i++){
      if(i>0 && keys[i] <= keys[i-1]) std::cout<<"Get reverse keys"<<std::endl;
    }

    lipp_index.bulk_load_disk(keys, values, count);
    learned.store(true);
    // return true;
  }
  else if(adgMod::modelmode == 2){
    string index_name = adgMod::db_name + "/" + filename + ".fmodel";
    char * index_name_p = &index_name[0];
    ft.error_bound = error;
    ft.sm = new ft::StorageManager(true, index_name_p);


    int count = string_keys.size();
    real_num_entries = count;
    ftKeyType *keys = new ftKeyType[count];
    for(int i=0; i<count; i++){
          keys[i] = stoll(string_keys[i].first);
      }

    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key >= thiskey){
          keys[i]= last_key +1;
        }
        else{
          keys[i] = thiskey;
        }
        last_key = keys[i];
    }

    std::vector<ftKeyType> data2(keys, keys+count);

    ft::Iterm *data =new ft::Iterm[count];
    for (int i = 0; i < count; i++) {
        data[i].key = stoll(string_keys[i].first);
        data[i].value = (long long)i;
        // std::cout<<i<<" "<<data[i].value<<std::endl;
    }
        
    ft.bulk_load_pgm(data, count, data2.begin(), data2.end(), error);
  }
  else if (adgMod::modelmode == 3){
    pgm::PGMIndex<uint64_t> index(keys.begin(), keys.end(),error,4);
    pgm = index;
    learned.store(true);
  }

  else if (adgMod::modelmode == 4){
    // std::cout<<"Training RMI models "<< filename <<"..."<<std::endl;
    string index_name = adgMod::db_name + "/" + filename + ".fmodel";
    std::size_t layer2_size = adgMod::rmi_layer_size;
    int count = string_keys.size();
    real_num_entries = count;
    std::vector<uint64_t> keys;
    // for(int i=0; i<count; i++){
    //   keys.push_back(stoll(string_keys[i].first));
    // }

    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key >= thiskey){
          keys.push_back(last_key +1);
          // cout<<"turns "<<last_key<<" to "<<keys[i]<<endl;
        }
        else{
          keys.push_back(thiskey);
        }
        last_key = keys[i];
    }
    rmi::RmiLAbs<uint64_t, rmi::LinearSpline, rmi::LinearRegression> rmi_(keys, layer2_size);
    // std::cout<<"Trained obj generated!"<<std::endl;
    rmi = rmi_;
    // std::cout<<"Object assigned!"<<std::endl;
    // rmi.printstats();
  }

  else if(adgMod::modelmode == 5){
    // std::cout<<"Training RS models "<< filename <<"..."<<std::endl;
    string index_name = adgMod::db_name + "/" + filename + ".fmodel";
    int count = string_keys.size();
    real_num_entries = count;
    std::vector<long long int> keys;
    // for(int i=0; i<count; i++){
    //   keys.push_back(stoll(string_keys[i].first));
    // }

    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key >= thiskey){
          keys.push_back(last_key +1);
          // cout<<"turns "<<last_key<<" to "<<keys[i]<<endl;
        }
        else{
          keys.push_back(thiskey);
        }
        last_key = keys[i];
    }

    long long int min = keys.front();
    long long int max = keys.back();
    rs::Builder<uint64_t> rsb(min, max, adgMod::RSbits, error);
    for (const auto& key : keys) rsb.AddKey(key);
    rs::RadixSpline<uint64_t> rs_;
    rs_ = rsb.Finalize();
    rs = rs_;
  }
  else if(adgMod::modelmode == 6){
    // std::cout<<"Training PLEX models "<< filename <<"..."<<std::endl;
    string index_name = adgMod::db_name + "/" + filename + ".fmodel";
    int count = string_keys.size();
    real_num_entries = count;
    std::vector<long long int> keys;
    // for(int i=0; i<count; i++){
    //   keys.push_back(stoll(string_keys[i].first));
    // }

    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key >= thiskey){
          keys.push_back(last_key +1);
          // cout<<"turns "<<last_key<<" to "<<keys[i]<<endl;
        }
        else{
          keys.push_back(thiskey);
        }
        last_key = keys[i];
    }
    long long int min = keys.front();
    long long int max = keys.back();
    ts::Builder<long long int> rsb(min, max, error);
    for (const auto& key : keys) rsb.AddKey(key);
    ts::TrieSpline<long long int> ts_;
    ts_ = rsb.Finalize();
    ts = ts_;
  }
  else if(adgMod::modelmode == 7){
    // std::cout<<"Training DILI models "<< filename <<"..."<<std::endl;
    string index_name = adgMod::db_name + "/" + filename + ".fmodel";
    int count = string_keys.size();
    real_num_entries = count;
    vector< pair<long long, int> > bulk_load_data_;
    // for(int i=0; i<count; i++){
    //   bulk_load_data_.push_back(std::make_pair(stoll(string_keys[i].first), i));
    // }
    if(count<8192){
      learned.store(false);
      return false;
  } 

    long long last_key = 0;
    for(int i=0; i<count; i++){
        long long thiskey = stoll(string_keys[i].first);
        if(i>0 && last_key >= thiskey){
          bulk_load_data_.push_back(std::make_pair(last_key+1 ,i));
          // cout<<"turns "<<last_key<<" to "<<keys[i]<<endl;
        }
        else{
          bulk_load_data_.push_back(std::make_pair(thiskey, i));
        }
        last_key = bulk_load_data_[i].first;
    }
    for(int i=0; i<count; i++){
      if(i>0 && bulk_load_data_[i].first <= bulk_load_data_[i-1].first) std::cout<<"Get reverse keys"<<std::endl;
    }

    string mirror_dir = adgMod::db_name + "/" + filename + ".mirror";
    dili.set_mirror_dir(mirror_dir);
    dili.bulk_load(bulk_load_data_);
    auto start = std::chrono::steady_clock::now();
    dili.save(index_name);
    auto end = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double, std::micro>(end - start).count();
    adgMod::write_model_duration+=duration;
  }
  else if(adgMod::modelmode == 8){
    
  }
  else if(adgMod::modelmode == 9)
  {
    string index_name = adgMod::db_name + "/" + filename + "_idx.fmodel";
    string data_name = adgMod::db_name + "/" + filename + "_dat.fmodel";
    char* index_name_p = (char*)index_name.data();
    char* data_name_p = (char*)data_name.data();
    int count = string_keys.size();
    std::vector<uint64_t> keys;
    for (auto it : string_keys)
    {
      keys.push_back(stoll(it.first));
    }
    FitingTree<uint64_t> ft(keys,error);
    ft_index = ft;
  }
  learned.store(true);
  if(!adgMod::fresh_write)
  {
    auto start=std::chrono::steady_clock::now();
    db->versions_->current()->WriteLevelModel();
    auto end = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double, std::micro>(end - start).count();
    adgMod::write_model_duration+=duration;
  }
  return true;

}

// static learning function to be used with LevelDB background scheduling
// level learning
void LearnedIndexData::LevelLearn(void* arg, bool nolock) {
  Stats* instance = Stats::GetInstance();
  bool success = false;
  bool entered = false;
  instance->StartTimer(8);

  VersionAndSelf* vas = reinterpret_cast<VersionAndSelf*>(arg);
  LearnedIndexData* self = vas->self;
  self->is_level = true;
  self->level = vas->level;
  Version* c;
  if (!nolock) {
    c = db->GetCurrentVersion();
  }
  if (db->version_count == vas->v_count) {
    entered = true;
    if (vas->version->FillLevel(adgMod::read_options, vas->level)) {
      self->filled = true;
      if (db->version_count == vas->v_count) {
        string filename = adgMod::db_name + "/" + to_string(vas->level) + ".model";
        if (env->compaction_awaiting.load() == 0 && self->Learn(filename)) {
          success = true;
        } else {
          self->learning.store(false);
        }
      }
    }
  }
  if (!nolock) {
    adgMod::db->ReturnCurrentVersion(c);
  }

  auto time = instance->PauseTimer(8, true);

  if (entered) {
    self->cost = time.second - time.first;
    learn_counter_mutex.Lock();
    events[1].push_back(new LearnEvent(time, 0, self->level, success));
    levelled_counters[6].Increment(vas->level, time.second - time.first);
    learn_counter_mutex.Unlock();
  }

  delete vas;
}

// static learning function to be used with LevelDB background scheduling
// file learning
uint64_t LearnedIndexData::FileLearn(void* arg) {
  Stats* instance = Stats::GetInstance();
  bool entered = false;
  instance->StartTimer(11);
  MetaAndSelf* mas = reinterpret_cast<MetaAndSelf*>(arg);
  LearnedIndexData* self = mas->self;
  self->level = mas->level;

  FileMetaData* meta = mas->meta;

  // std::cout<<"Learning File:"<<fimename<<std::endl;

  // Version* c = db->GetCurrentVersion();
  Version* c = db->versions_->current();
  c->Ref();
  if (self->FillData(c, mas->meta)) {
    self->LearnFileNew(self->keys, self->level);
    self->WriteLearnedModelNew(adgMod::db_name+"/"+std::to_string(meta->number)+".fmodel");
    adgMod::num_entry_map.insert(make_pair(meta->number,self->keys.size()));
    entered = true;
  } else {
    // std::cout<<"Data NOT Filled"<<std::endl;
    self->learning.store(false);
  }
  // adgMod::db->ReturnCurrentVersion(c);
  c->Unref();

  auto time = instance->PauseTimer(11, true);

  if (entered) {
    // count how many file learning are done.
    self->cost = time.second - time.first;
    learn_counter_mutex.Lock();
    events[1].push_back(new LearnEvent(time, 1, self->level, true));
    levelled_counters[11].Increment(mas->level, time.second - time.first);
    learn_counter_mutex.Unlock();
  }

  //        if (fresh_write) {
  //            self->WriteModel(adgMod::db->versions_->dbname_ + "/" +
  //            to_string(mas->meta->number) + ".fmodel");
  //            self->string_keys.clear();
  //            self->num_entries_accumulated.array.clear();
  //        }

  if (!fresh_write) delete mas->meta;
  delete mas;
  return entered ? time.second - time.first : 0;
}

// general model checker
bool LearnedIndexData::Learned() {
  if (learned_not_atomic)
    return true;
  else if (learned.load()) {
    learned_not_atomic = true;
    return true;
  } else
    return false;
}

// level model checker, used to be also learning trigger
bool LearnedIndexData::Learned(Version* version, int v_count, int level) {
  // std::cout<<"learned_not_atomic:";
  // std::cout<<learned_not_atomic<<std::endl;
  if (learned_not_atomic)
    return true;
  else if (learned.load()) {
    learned_not_atomic = true;
    return true;
  }
  return false;
}

// file model checker, used to be also learning trigger
bool LearnedIndexData::Learned(Version* version, int v_count,
                               FileMetaData* meta, int level) {
  // std::cout<< " !learning.exchange(true) "<< !learning.exchange(true)<<std::endl;
  if (learned_not_atomic)
    return true;
  else if (learned.load()) {
    learned_not_atomic = true;
    return true;
  } else
    return false;
}

bool LearnedIndexData::FillData(Version* version, FileMetaData* meta) {
  // if (filled) return true;

  if (version->FillData(adgMod::read_options, meta, this)) {
    // std::cout<<"\tData Filled"<<std::endl;
    // filled = true;
    return true;
  }
  // std::cout<<"\tData NOT Filled"<<std::endl;
  return false;
}

void LearnedIndexData::WriteModel(const string& filename) {

  if(adgMod::modelmode == 0){
    if (!learned.load()) return;

    std::ofstream output_file(filename);
    output_file.precision(40);
    output_file << adgMod::block_num_entries << " " << adgMod::block_size << " "
                << adgMod::entry_size << "\n";
    for (Segment& item : string_segments) {
      output_file << item.x << " " << item.k << " " << item.b << "\n";
    }
    output_file << "StartAcc"
                << " " << min_key << " " << max_key << " " << size << " " << level
                << " " << cost << "\n";
    output_file.close();
  }
  else if(adgMod::modelmode == 4){
    if(rmi.layer2_size() != 0){
          // std::cout<<"try write rmi model: "<< filename <<std::endl;
    string filename_ = filename;
    // rmi.printstats();
    rmi.write_file_e(filename_);
    }
  }
  else if(adgMod::modelmode == 5){
    rs::Serializer<uint64_t> serializer;
    std::string bytes;
    serializer.ToBytes(rs, &bytes);
    string filename_ = filename;
    std::ofstream outFile(filename_);
    outFile << bytes;
    outFile.close();
  }
  else if(adgMod::modelmode == 6){
    ts::Serializer<long long int> serializer;
    std::string bytes;
    serializer.ToBytes(ts, &bytes);
    string filename_ = filename;
    std::ofstream outFile(filename_);
    outFile << bytes;
    outFile.close();
  }
  // else if(adgMod::modelmode == 7){
  //   // if (!learned.load()) return;
  //   if(dili.root == NULL) return;
  //   dili.save(filename);
  // }
  return;
}

void LearnedIndexData::ReadModel(const string& filename) {

  std::ifstream infile;
  std::size_t pos = filename.rfind(".fmodel");
  std::string new_filename = filename.substr(0, pos) + ".fnum";
  infile.open(new_filename);
  if (infile.is_open())
  {
      int count;
      infile >> count;
      infile.close();
      real_num_entries = count;
      // std::cout<<"got real num keys:"<<real_num_entries<<std::endl;
  }
  // else
  // {
  //     std::cout << "Unable to open file";
  // }
  if(adgMod::modelmode == 0){

    std::ifstream input_file(filename, std::ios::binary | std::ios::ate);
    if (!input_file.good()) return;

    // 获取文件大小
    std::streamsize file_size = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    // 分配缓冲区并一次性读取整个文件
    std::vector<char> buffer(file_size);
    if (!input_file.read(buffer.data(), file_size)) {
        return; // 读取失败
    }

    // 在内存中解析缓冲区数据
    std::istringstream in(std::string(buffer.data(), buffer.size()));

    // 读取数据
    in >> adgMod::block_num_entries >> adgMod::block_size >> adgMod::entry_size;

    while (true) {
        std::string x;
        double k, b;
        in >> x;
        if (x == "StartAcc") break;
        in >> k >> b;
        string_segments.emplace_back(std::stoll(x), k, b);
    }

    in >> min_key >> max_key >> size >> level >> cost;

    // testing num entires
    // while (true) {
    //   uint64_t first;
    //   std::string second;
    //   if (!(in >> first >> second)) break;
    //   num_entries_accumulated.Add(first, std::move(second));
    // }
    // std::ifstream input_file(filename);
    // // std::cout<<"Reading model: "<<filename<<std::endl;

    // if (!input_file.good()) return;

    // input_file >> adgMod::block_num_entries >> adgMod::block_size >>
    //     adgMod::entry_size;
    // while (true) {
    //   string x;
    //   double k, b;
    //   input_file >> x;
    //   // std::cout<<x<<std::endl;
    //   if (x == "StartAcc") break;
    //   input_file >> k >> b;
    //   string_segments.emplace_back(atoll(x.c_str()), k, b);
    // }
    // input_file >> min_key >> max_key >> size >> level >> cost;
    // // testing num entires
    // // while (true) {
    // //   uint64_t first;
    // //   string second;
    // //   if (!(input_file >> first >> second)) break;
    // //   num_entries_accumulated.Add(first, std::move(second));
    // // }
  }
  else if(adgMod::modelmode == 1){
    std::ifstream input_file(filename);
    if (!input_file.good()) return;
    const char* index_name_p = filename.c_str();
    lipp_index.init(index_name_p, false);
    lipp_index.cal_total_block();
    lipp_index.readall();
  }
  else if(adgMod::modelmode == 2){
    std::ifstream input_file(filename);
    if (!input_file.good()) return;
    char* index_name_p = const_cast <char *>(filename.c_str());
    // std::cout<<index_name_p<<std::endl;
    ft.sm = new ft::StorageManager(false, index_name_p);
    // std::cout<<"here"<<std::endl;
    ft.error_bound = adgMod::file_model_error;
    ft.load_metanode();
  }
  else if(adgMod::modelmode == 3){
    
  }
  else if(adgMod::modelmode == 4){
    std::ifstream input_file(filename);
    if (!input_file.good()) return;
    rmi::RmiLAbs<uint64_t, rmi::LinearSpline, rmi::LinearRegression> rmi_;
    // std::cout<<"Begin reading...:"<<std::endl;
    rmi_.read_file_e(filename);
    rmi = rmi_;
  }
  else if(adgMod::modelmode == 5){

    std::ifstream input_file(filename, std::ios::binary | std::ios::ate);
    if (!input_file.good()) return;

    // 获取文件大小
    std::streamsize fileSize = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    // 创建一个缓冲区来存储整个文件内容
    std::vector<char> buffer(fileSize);

    // 读取整个文件到缓冲区
    if (!input_file.read(buffer.data(), fileSize)) {
        std::cerr << "Error reading file!" << std::endl;
        return;
    }

    input_file.close();

    // 将缓冲区内容转换为字符串
    std::string fileContent(buffer.begin(), buffer.end());

    // 反序列化
    rs::Serializer<uint64_t> serializer;
    const auto rs_deserialized = serializer.FromBytes(fileContent);
    rs = rs_deserialized;

    // std::ifstream input_file(filename);
    // if (!input_file.good()) return;
    // std::string fileContent;
    // fileContent = string((std::istreambuf_iterator<char>(input_file)),
    //               std::istreambuf_iterator<char>());
    // input_file.close();
    // rs::Serializer<long long int> serializer;
    // const auto rs_deserialized = serializer.FromBytes(fileContent);
    // rs = rs_deserialized;
  }
  else if(adgMod::modelmode == 6){

    std::ifstream input_file(filename, std::ios::binary | std::ios::ate);
    if (!input_file.good()) return;

    // 获取文件大小
    std::streamsize fileSize = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    // 创建一个缓冲区来存储整个文件内容
    std::vector<char> buffer(fileSize);

    // 读取整个文件到缓冲区
    if (!input_file.read(buffer.data(), fileSize)) {
        std::cerr << "Error reading file!" << std::endl;
        return;
    }

    input_file.close();

    // 将缓冲区内容转换为字符串
    std::string fileContent(buffer.begin(), buffer.end());

    // std::ifstream input_file(filename);
    // if (!input_file.good()) return;
    // std::string fileContent;
    // fileContent = string((std::istreambuf_iterator<char>(input_file)),
    //               std::istreambuf_iterator<char>());
    // input_file.close();
    ts::Serializer<long long int> serializer;
    const auto rs_deserialized = serializer.FromBytes(fileContent);
    ts = rs_deserialized;
  }
  else if(adgMod::modelmode == 7){
    std::ifstream input_file(filename);
    if (!input_file.good()) return;
    // std::cout<<"opening file after reopen db "<<filename<<std::endl;
    DILI dili2;
    dili2.load(filename);
    dili = dili2;
  }
  else if(adgMod::modelmode == 8){

    string file_name_s = filename;

    file_name_s.erase(file_name_s.size()-7);
    string index_name = file_name_s + "_idx.fmodel";
    string data_name = file_name_s + "_dat.fmodel";
    char* index_name_p = (char*)index_name.data();
    char* data_name_p = (char*)data_name.data();
    // std::cout<<"opening file after reopen db "<<index_name_p<<std::endl;
    std::ifstream input_file(index_name);
    if (!input_file.good()) return;
    // std::cout<<"again opening file after reopen db "<<index_name_p<<std::endl;
    alex::Alex<long long, int> index2(0, false, index_name_p, data_name_p);
    alex_index = index2;

    auto stats = alex_index.get_stats();
    // std::cout<<" file_name_s:" << file_name_s <<" keys: "<<stats.num_keys<<" inner nodes: "<<stats.num_model_nodes<<" data nodes: "<<stats.num_data_nodes<<std::endl;
  }



  learned.store(true);
}

void LearnedIndexData::ReportStats() {
  //        double neg_gain, pos_gain;
  //        if (num_neg_model == 0 || num_neg_baseline == 0) {
  //            neg_gain = 0;
  //        } else {
  //            neg_gain = ((double) time_neg_baseline / num_neg_baseline -
  //            (double) time_neg_model / num_neg_model) * num_neg_model;
  //        }
  //        if (num_pos_model == 0 || num_pos_baseline == 0) {
  //            pos_gain = 0;
  //        } else {
  //            pos_gain = ((double) time_pos_baseline / num_pos_baseline -
  //            (double) time_pos_model / num_pos_model) * num_pos_model;
  //        }

  printf("%d %d %lu %lu %lu\n", level, served, string_segments.size(), cost,
         size);  //, file_size);
  //        printf("\tPredicted: %lu %lu %lu %lu %d %d %d %d %d %lf\n",
  //        time_neg_baseline_p, time_neg_model_p, time_pos_baseline_p,
  //        time_pos_model_p,
  //                num_neg_baseline_p, num_neg_model_p, num_pos_baseline_p,
  //                num_pos_model_p, num_files_p, gain_p);
  //        printf("\tActual: %lu %lu %lu %lu %d %d %d %d %f\n",
  //        time_neg_baseline, time_neg_model, time_pos_baseline,
  //        time_pos_model,
  //               num_neg_baseline, num_neg_model, num_pos_baseline,
  //               num_pos_model, pos_gain + neg_gain);
}

void LearnedIndexData::FillCBAStat(bool positive, bool model, uint64_t time) {
  //        int& num_to_update = positive ? (model ? num_pos_model :
  //        num_pos_baseline) : (model ? num_neg_model : num_neg_baseline);
  //        uint64_t& time_to_update =  positive ? (model ? time_pos_model :
  //        time_pos_baseline) : (model ? time_neg_model : time_neg_baseline);
  //        time_to_update += time;
  //        num_to_update += 1;
}

LearnedIndexData* FileLearnedIndexData::GetModel(int number) {
  leveldb::MutexLock l(&mutex);
  if (file_learned_index_data.size() <= number)
    file_learned_index_data.resize(number + 1, nullptr);
  if (file_learned_index_data[number] == nullptr)
    file_learned_index_data[number] = new LearnedIndexData(file_allowed_seek, false);
  return file_learned_index_data[number];
}

bool FileLearnedIndexData::FillData(Version* version, FileMetaData* meta) {
  LearnedIndexData* model = GetModel(meta->number);
  return model->FillData(version, meta);
}

std::vector<std::pair<std::string,int>>& FileLearnedIndexData::GetData(FileMetaData* meta) {
  auto* model = GetModel(meta->number);
  return model->string_keys;
}

bool FileLearnedIndexData::Learned(Version* version, FileMetaData* meta,
                                   int level) {
  LearnedIndexData* model = GetModel(meta->number);
  return model->Learned(version, db->version_count, meta, level);
}

AccumulatedNumEntriesArray* FileLearnedIndexData::GetAccumulatedArray(
    int file_num) {
  auto* model = GetModel(file_num);
  return &model->num_entries_accumulated;
}

std::pair<uint64_t, uint64_t> FileLearnedIndexData::GetPosition(
    const Slice& key, int file_num) {
  return file_learned_index_data[file_num]->GetPosition(key);
}

FileLearnedIndexData::~FileLearnedIndexData() {
  leveldb::MutexLock l(&mutex);
  for (auto pointer : file_learned_index_data) {
    delete pointer;
  }
}

long long int FileLearnedIndexData::Getmodelsize() {

  if(adgMod::modelmode==0){
    // std::cout<<"in branch 0"<<std::endl;
    int segsum = 0;
    int acc_arr_size = 0;
    leveldb::MutexLock l(&mutex);

    std::set<uint64_t> live_files;
    adgMod::db->versions_->AddLiveFiles(&live_files);

    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        // if (pointer != nullptr && pointer->cost != 0) {
        if (pointer != nullptr) {
            if(!pointer->Learned()) continue;
            // printf("FileModel %lu %d ", i, i > watermark);
            segsum += pointer->string_segments.size(); //in seg num
            // acc_arr_size += pointer->num_entries_accumulated.array.size();
        }
    }
    // std::cout<<"acc size:"<<acc_arr_size<<" segnum:"<<segsum<<std::endl;
    std::cout<<"Segsum:"<<segsum<<std::endl;
    return (long long)(segsum * 3 * 8 + acc_arr_size * (8 + adgMod::key_size)); //in segnum
  }
  else if(adgMod::modelmode==1){
    std::cout<<"in branch 1"<<std::endl;
    long long int size_byte = 0;
    leveldb::MutexLock l(&mutex);

    std::set<uint64_t> live_files;
    adgMod::db->versions_->AddLiveFiles(&live_files);

    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
      
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
            if(!pointer->Learned()) continue;
            size_byte += pointer->lipp_index.cal_lipp_size(); //in seg num
            // std::cout<<"in getmodelsize(): "<<pointer->lipp_index.cal_lipp_size()<<std::endl;
            // acc_arr_size += pointer->num_entries_accumulated.array.size();
        }
    }
    return size_byte;
  }
  else if(adgMod::modelmode==2){
    long long int size_byte = 0;
    leveldb::MutexLock l(&mutex);

    std::set<uint64_t> live_files;
    adgMod::db->versions_->AddLiveFiles(&live_files);

    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->ft_index.get_index_size();
        }
    }
    return size_byte;

  }
  else if(adgMod::modelmode==3){
    long long int size_byte = 0;
    leveldb::MutexLock l(&mutex);

    std::set<uint64_t> live_files;
    adgMod::db->versions_->AddLiveFiles(&live_files);

    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->pgm.get_index_size();
        }
    }
    return size_byte;
  }
  else if(adgMod::modelmode == 4){
    std::size_t size_byte = 0;
    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->rmi.size_in_bytes();
        }
    }
    return (long long int)size_byte;
  }
  else if(adgMod::modelmode == 5){
    std::size_t size_byte = 0;
    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->rs.GetSize();
        }
    }
    return (long long int)size_byte;
  }
  else if(adgMod::modelmode == 6){
    std::size_t size_byte = 0;
    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->ts.GetSize();
        }
    }
    return (long long int)size_byte;
  }
  else if(adgMod::modelmode == 7){
    std::size_t size_byte = 0;
    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->dili.size();
        }
    }
    return (long long int)size_byte;
  }
  else if(adgMod::modelmode == 8){
    std::size_t size_byte = 0;
    for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
        auto pointer = file_learned_index_data[i];
        
        if (pointer != nullptr) {
          if(!pointer->Learned()) continue;
          size_byte += pointer->alex_index.get_file_size();
        }
    }
    return (long long int)size_byte;

  }
}

void FileLearnedIndexData::Report() {
  // printf("Getting alive files!");

  leveldb::MutexLock l(&mutex);

  std::set<uint64_t> live_files;
  // printf("Getting alive files!");
  adgMod::db->versions_->AddLiveFiles(&live_files);
  int segsum = 0;
  size_t lipp_size = 0;
  // printf("Into loop to check file models");
  for (size_t i = 0; i < file_learned_index_data.size(); ++i) {
    auto pointer = file_learned_index_data[i];

    // 
    // printf("Current i:%d\n ", i);
    if (pointer != nullptr && pointer->cost != 0 ) {
      // std::cout<<i<<" "<<pointer->Learned()<<std::endl;

      // printf("FileModel %lu %d ", i, i > watermark);
      if(pointer->Learned()){
        if(adgMod::modelmode==0){
          pointer->ReportStats();
          segsum += pointer->string_segments.size();
        }
        else if(adgMod::modelmode==1){
          std::cout<<"FileModel: "<<i<<std::endl;
          std::cout<<"FileModel: "<<i<<" lipp size:"<<pointer->lipp_index.cal_lipp_size()<<std::endl;
          lipp_size += pointer->lipp_index.cal_lipp_size();
        }
        else if(adgMod::modelmode==2){
          std::cout<<"FileModel: "<<i<<" ft size:"<<pointer->ft.get_index_size()<<std::endl;
        }
        else if(adgMod::modelmode==3){
          // std::cout<<"FileModel: "<<i<<" PGM size:"<<pointer->pgm.file_size_in_bytes()<<std::endl;
        }
        else if(adgMod::modelmode==4){
          std::cout<<"FileModel: "<<i<<" RMI size:"<<pointer->rmi.size_in_bytes()<<std::endl;
        }
        else if(adgMod::modelmode==5){
          std::cout<<"FileModel: "<<i<<" RS size:"<<pointer->rs.GetSize()<<std::endl;
        }
        else if(adgMod::modelmode==6){
          std::cout<<"FileModel: "<<i<<" TS size:"<<pointer->ts.GetSize()<<std::endl;
        }
        else if(adgMod::modelmode==7){
          std::cout<<"FileModel: "<<i<<" DILI size:"<<pointer->dili.size()<<std::endl;
        }
        else if(adgMod::modelmode==8){
          std::cout<<"FileModel: "<<i<<" ALEX size:"<<pointer->alex_index.get_file_size()<<std::endl;
        }
      }
      
      
    }
  }
  printf("segsum: %d\n", segsum);
}

void AccumulatedNumEntriesArray::Add(uint64_t num_entries, string&& key) {
  array.emplace_back(num_entries, key);
}

bool AccumulatedNumEntriesArray::Search(const Slice& key, uint64_t lower,
                                        uint64_t upper, size_t* index,
                                        uint64_t* relative_lower,
                                        uint64_t* relative_upper) {
  if (adgMod::MOD == 4) {
    uint64_t lower_pos = lower / array[0].first;
    uint64_t upper_pos = upper / array[0].first;
    if (lower_pos != upper_pos) {
      while (true) {
        if (lower_pos >= array.size()) return false;
        if (key <= array[lower_pos].second) break;
        lower = array[lower_pos].first;
        ++lower_pos;
      }
      upper = std::min(upper, array[lower_pos].first - 1);
      *index = lower_pos;
      *relative_lower =
          lower_pos > 0 ? lower - array[lower_pos - 1].first : lower;
      *relative_upper =
          lower_pos > 0 ? upper - array[lower_pos - 1].first : upper;
      return true;
    }
    *index = lower_pos;
    *relative_lower = lower % array[0].first;
    *relative_upper = upper % array[0].first;
    return true;

  } else {
    size_t left = 0, right = array.size() - 1;
    while (left < right) {
      size_t mid = (left + right) / 2;
      if (lower < array[mid].first)
        right = mid;
      else
        left = mid + 1;
    }

    if (upper >= array[left].first) {
      while (true) {
        if (left >= array.size()) return false;
        if (key <= array[left].second) break;
        lower = array[left].first;
        ++left;
      }
      upper = std::min(upper, array[left].first - 1);
    }

    *index = left;
    *relative_lower = left > 0 ? lower - array[left - 1].first : lower;
    *relative_upper = left > 0 ? upper - array[left - 1].first : upper;
    return true;
  }
}

bool AccumulatedNumEntriesArray::SearchNoError(uint64_t position, size_t* index,
                                               uint64_t* relative_position) {
  *index = position / array[0].first;
  *relative_position = position % array[0].first;
  return *index < array.size();

  //        size_t left = 0, right = array.size() - 1;
  //        while (left < right) {
  //            size_t mid = (left + right) / 2;
  //            if (position < array[mid].first) right = mid;
  //            else left = mid + 1;
  //        }
  //        *index = left;
  //        *relative_position = left > 0 ? position - array[left - 1].first :
  //        position; return left < array.size();
}

uint64_t AccumulatedNumEntriesArray::NumEntries() const {
  return array.empty() ? 0 : array.back().first;
}

void LearnedIndexData::LearnFileNew(const std::vector<uint64_t>& keys, int level) {
  auto start = std::chrono::steady_clock::now();
  auto level_error = error;
  for (int i = 0; i < level; i++) level_error *= error_multiplier;
  real_num_entries = keys.size();
  size=keys.size();
  if (adgMod::modelmode == 0) {
    // PLR
    PLR plr = PLR(error);
    uint64_t temp = keys.back();
    min_key = keys.front();
    max_key = temp;
    size = keys.size();
    uint64_t last_key = 0;
    auto cur_keys = keys;
    std::vector<std::pair<std::string, int>> skeys;
    for (size_t i = 0; i < cur_keys.size(); ++i) {
      long long thiskey = cur_keys[i];
      if (i > 0 && last_key == thiskey) {
        cur_keys[i] = last_key + 1;
      }
      last_key = cur_keys[i];
      int is_first = i == 0;
      skeys.push_back({std::to_string(cur_keys[i]), is_first});
    }
    std::vector<Segment> segments = plr.train(skeys, true);
    if (segments.empty()) return;
    segments.push_back((Segment){temp, 0, 0});
    string_segments = std::move(segments);
  }
  else if (adgMod::modelmode == 2)
  {
    FitingTree<uint64_t> ft(keys, level_error);
    ft_index = ft;
  }
  else if (adgMod::modelmode == 3) {
    // pgm
    pgm::PGMIndex<uint64_t> index(keys.begin(), keys.end(),error,adgMod::epsilonR);
    pgm = index;
  }
  else if (adgMod::modelmode == 4)
  {
    rmi::RmiLAbs<uint64_t, rmi::LinearSpline, rmi::LinearRegression> rmi_(keys, adgMod::rmi_layer_size);
    rmi = rmi_;
  }
  else if (adgMod::modelmode == 5)
  {
    uint64_t min = keys.front();
    uint64_t max = keys.back();
    rs::Builder<uint64_t> rsb(min, max, adgMod::RSbits, error);
    for (const auto& key : keys) rsb.AddKey(key);
    rs = rsb.Finalize();
  }
  else if (adgMod::modelmode == 6) {
    ts::Builder<long long int> rsb(keys.front(), keys.back(), error);
    for (const auto& key : keys) rsb.AddKey(key);
    ts = rsb.Finalize();
  }
  learned.store(true);
  auto end = std::chrono::steady_clock::now();
  if (!adgMod::fresh_write)
    adgMod::learn_duration += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    adgMod::filelearn_count++;
}

void LearnedIndexData::WriteLearnedModelNew(const std::string& filename) {
  auto start = std::chrono::steady_clock::now();
  if (adgMod::modelmode == 0) {
    // PLR
    std::ofstream output_file(filename, std::ios::binary);
    output_file.write(reinterpret_cast<const char*>(&min_key), sizeof(min_key));
    output_file.write(reinterpret_cast<const char*>(&max_key), sizeof(max_key));
    output_file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    int seg_size = string_segments.size();
    output_file.write(reinterpret_cast<const char*>(&seg_size), sizeof(seg_size));
    for (auto& segment : string_segments) {
      output_file.write(reinterpret_cast<const char*>(&segment.x), sizeof(segment.x));
      output_file.write(reinterpret_cast<const char*>(&segment.k), sizeof(segment.k));
      output_file.write(reinterpret_cast<const char*>(&segment.b), sizeof(segment.b));
    }
    // output_file.precision(40);
    // // 1. min key
    // output_file << min_key << std::endl;
    // // 2. max key
    // output_file << max_key << std::endl;
    // // 3. size
    // output_file << size << std::endl;
    // // 4. segments
    // output_file << string_segments.size() << std::endl;
    // for (auto& segment : string_segments) {
    //   output_file << segment.x << " " << segment.k << " " << segment.b << std::endl;
    // }
  }
  if (adgMod::modelmode == 2)
  {
    std::ofstream output_file(filename);
    ft_index.dump_model(output_file);
    output_file.close();
  }
  if (adgMod::modelmode == 3) {
    // pgm
    std::ofstream output_file(filename, std::ios::binary);
    output_file.write(reinterpret_cast<const char*>(&pgm.Epsilon), sizeof(pgm.Epsilon));
    output_file.write(reinterpret_cast<const char*>(&pgm.EpsilonRecursive), sizeof(pgm.EpsilonRecursive));
    output_file.write(reinterpret_cast<const char*>(&pgm.n), sizeof(pgm.n));
    output_file.write(reinterpret_cast<const char*>(&pgm.first_key), sizeof(pgm.first_key));
    int seg_size = pgm.segments.size();
    output_file.write(reinterpret_cast<const char*>(&seg_size), sizeof(seg_size));
    for (auto& segment : pgm.segments) {
      output_file.write(reinterpret_cast<const char*>(&segment.key), sizeof(segment.key));
      output_file.write(reinterpret_cast<const char*>(&segment.slope), sizeof(segment.slope));
      output_file.write(reinterpret_cast<const char*>(&segment.intercept), sizeof(segment.intercept));
    }
    int lvl_size = pgm.levels_offsets.size();
    output_file.write(reinterpret_cast<const char*>(&lvl_size), sizeof(lvl_size));
    for (auto& offset : pgm.levels_offsets) {
      output_file.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
    }
    output_file.close();
  }
  else if (adgMod::modelmode == 4)
  {
    std::ofstream output_file(filename);
    rmi.dump(output_file);
    output_file.close();
  }
  else if (adgMod::modelmode == 5)
  {
    rs::Serializer<uint64_t> serializer;
    std::string bytes;
    serializer.ToBytes(rs, &bytes);
    std::ofstream outFile(filename);
    outFile << bytes;
    outFile.close();
  }
  else if (adgMod::modelmode == 6) {
    ts::Serializer<long long int> serializer;
    std::string bytes;
    serializer.ToBytes(ts, &bytes);
    std::ofstream outFile(filename);
    outFile << bytes;
    outFile.close();
  }
  auto end = std::chrono::steady_clock::now();
  if (!adgMod::fresh_write)
    adgMod::write_model_duration += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

void LearnedIndexData::LoadLearnedModelNew(const std::string& filename) {
  if (adgMod::modelmode == 0) {
    // PLR
    std::ifstream input_file(filename, std::ios::binary);
    if (!input_file.good()) return;
    // load members
    // input_file >> min_key;
    // input_file >> max_key;
    // input_file >> size;
    // int seg_size = 0;
    // input_file >> seg_size;
    // for (int i = 0; i < seg_size; ++i) {
    //   uint64_t x;
    //   double k, b;
    //   input_file >> x >> k >> b;
    //   string_segments.emplace_back(x, k, b);
    // }
    input_file.read(reinterpret_cast<char*>(&min_key), sizeof(min_key));
    input_file.read(reinterpret_cast<char*>(&max_key), sizeof(max_key));
    input_file.read(reinterpret_cast<char*>(&size), sizeof(size));
    int seg_size = 0;
    input_file.read(reinterpret_cast<char*>(&seg_size), sizeof(seg_size));
    for (int i = 0; i < seg_size; ++i) {
      uint64_t x;
      double k, b;
      input_file.read(reinterpret_cast<char*>(&x), sizeof(x));
      input_file.read(reinterpret_cast<char*>(&k), sizeof(k));
      input_file.read(reinterpret_cast<char*>(&b), sizeof(b));
      string_segments.emplace_back(x, k, b);
    }
    input_file.close();
  }
  else if (adgMod::modelmode == 2)
  {
    std::ifstream input_file(filename, std::ios::binary);
    if (!input_file.good()) return;
    ft_index.load_model(input_file);
    input_file.close();
  }
  else if (adgMod::modelmode == 3) {
    // pgm
    std::ifstream input_file(filename, std::ios::binary);
    if (!input_file.good()) return;
    pgm = pgm::PGMIndex<uint64_t>();
    input_file.read(reinterpret_cast<char*>(&pgm.Epsilon), sizeof(pgm.Epsilon));
    input_file.read(reinterpret_cast<char*>(&pgm.EpsilonRecursive), sizeof(pgm.EpsilonRecursive));
    input_file.read(reinterpret_cast<char*>(&pgm.n), sizeof(pgm.n));
    input_file.read(reinterpret_cast<char*>(&pgm.first_key), sizeof(pgm.first_key));
    int seg_size = 0;
    input_file.read(reinterpret_cast<char*>(&seg_size), sizeof(seg_size));
    for (int i = 0; i < seg_size; ++i) {
      uint64_t key;
      float slope;
      int32_t intercept;
      input_file.read(reinterpret_cast<char*>(&key), sizeof(key));
      input_file.read(reinterpret_cast<char*>(&slope), sizeof(slope));
      input_file.read(reinterpret_cast<char*>(&intercept), sizeof(intercept));
      pgm.segments.push_back({key, slope, intercept});
    }
    int lvl_size = 0;
    input_file.read(reinterpret_cast<char*>(&lvl_size), sizeof(lvl_size));
    for (int i = 0; i < lvl_size; ++i) {
      uint64_t offset;
      input_file.read(reinterpret_cast<char*>(&offset), sizeof(offset));
      pgm.levels_offsets.push_back(offset);
    }
    input_file.close();
  }
  else if (adgMod::modelmode == 4)
  {
    std::ifstream input_file(filename, std::ios::binary);
    if (!input_file.good()) return;
    rmi::RmiLAbs<uint64_t, rmi::LinearSpline, rmi::LinearRegression> rmi_;
    rmi_.load(input_file);
    rmi = rmi_;
    input_file.close();
  }
  else if (adgMod::modelmode == 5)
  {
    std::ifstream input_file(filename, std::ios::binary | std::ios::ate);
    if (!input_file.good()) return;

    // 获取文件大小
    std::streamsize fileSize = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    // 创建一个缓冲区来存储整个文件内容
    std::vector<char> buffer(fileSize);

    // 读取整个文件到缓冲区
    if (!input_file.read(buffer.data(), fileSize)) {
        std::cerr << "Error reading file!" << std::endl;
        return;
    }

    input_file.close();

    // 将缓冲区内容转换为字符串
    std::string fileContent(buffer.begin(), buffer.end());

    // 反序列化
    rs::Serializer<uint64_t> serializer;
    const auto rs_deserialized = serializer.FromBytes(fileContent);
    rs = rs_deserialized;
  }
  else if (adgMod::modelmode == 6) {
    std::ifstream input_file(filename, std::ios::binary | std::ios::ate);
    if (!input_file.good()) return;

    // 获取文件大小
    std::streamsize fileSize = input_file.tellg();
    input_file.seekg(0, std::ios::beg);

    // 创建一个缓冲区来存储整个文件内容
    std::vector<char> buffer(fileSize);

    // 读取整个文件到缓冲区
    if (!input_file.read(buffer.data(), fileSize)) {
      std::cerr << "Error reading file!" << std::endl;
      return;
    }
    input_file.close();

    // 将缓冲区内容转换为字符串
    std::string fileContent(buffer.begin(), buffer.end());
    ts::Serializer<long long int> serializer;
    const auto rs_deserialized = serializer.FromBytes(fileContent);
    ts = rs_deserialized;
  }
  learned.store(true);
}

}  // namespace adgMod
