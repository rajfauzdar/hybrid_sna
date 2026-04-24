#include <iostream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <string>
#include <hiredis/hiredis.h>
#include <unistd.h>

using namespace std;

int worker_id;
string my_queue, other_queue;
unordered_map<int, int> local_degree; // node -> degree count

bool is_local(int node) {
    return (node % 2 == worker_id - 1);
}

void load_partition(const string& filename) {
    ifstream infile(filename);
    int u, v;
    while (infile >> u >> v) {
        // Count degree for local nodes
        if (is_local(u)) local_degree[u]++;
        if (is_local(v)) local_degree[v]++;
    }
    cout << "Loaded " << local_degree.size() << " local nodes." << endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./degree <worker_id> <partition_file>" << endl;
        return 1;
    }

    worker_id = atoi(argv[1]);
    string partition_file = argv[2];

    my_queue    = (worker_id == 1) ? "degree_baadal" : "degree_gcp";
    other_queue = (worker_id == 1) ? "degree_gcp"    : "degree_baadal";

    // Connect to Redis
    redisContext* redis = redisConnect("100.98.11.34", 6379);
    if (!redis || redis->err) {
        cerr << "Redis connection failed!" << endl;
        return 1;
    }

    if (worker_id == 1) {
        redisCommand(redis, "FLUSHALL");
        cout << "Redis cleared." << endl;
    }

    // READY BARRIER
    redisCommand(redis, "INCR degree_ready");
    cout << "Waiting for both workers..." << endl;
    while (true) {
        redisReply* r = (redisReply*)redisCommand(redis, "GET degree_ready");
        if (r && r->str && atoi(r->str) >= 2) { freeReplyObject(r); break; }
        if (r) freeReplyObject(r);
        usleep(200000);
    }
    cout << "Both workers ready! Computing degree centrality..." << endl;

    // PHASE 1: Load partition and count local degrees
    load_partition(partition_file);

    // PHASE 2: Send all local degrees to other worker via Redis
    cout << "Sharing degree counts..." << endl;
    vector<string> msg_strs;
    vector<const char*> argv_r;
    vector<size_t> argvlen;

    argv_r.push_back("LPUSH");
    argvlen.push_back(5);
    argv_r.push_back(other_queue.c_str());
    argvlen.push_back(other_queue.size());

    for (auto& pair : local_degree) {
        msg_strs.push_back(to_string(pair.first) + ":" + to_string(pair.second));
    }
    for (auto& msg : msg_strs) {
        argv_r.push_back(msg.c_str());
        argvlen.push_back(msg.size());
    }

    redisReply* r = (redisReply*)redisCommandArgv(redis, argv_r.size(), argv_r.data(), argvlen.data());
    if (r) freeReplyObject(r);

    // Signal done sending
    redisCommand(redis, "SET %s_done 1", my_queue.c_str());
    cout << "Sent " << local_degree.size() << " degree counts." << endl;

    // PHASE 3: Wait for other worker to finish sending
    string other_done = other_queue + "_done";
    while (true) {
        redisReply* r = (redisReply*)redisCommand(redis, "GET %s", other_done.c_str());
        if (r && r->str && atoi(r->str) == 1) { freeReplyObject(r); break; }
        if (r) freeReplyObject(r);
        usleep(100000);
    }

    // PHASE 4: Fetch other worker's degrees in one shot
    redisReply* all = (redisReply*)redisCommand(redis, "LRANGE %s 0 -1", my_queue.c_str());
    if (all && all->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < all->elements; i++) {
            if (!all->element[i]->str || all->element[i]->len == 0) continue;
            string msg = all->element[i]->str;
            int delim = msg.find(':');
            if (delim == (int)string::npos) continue;
            int node = stoi(msg.substr(0, delim));
            int deg  = stoi(msg.substr(delim + 1));
            // Merge — add to existing or insert
            local_degree[node] += deg;
        }
        freeReplyObject(all);
    }

    // PHASE 5: Worker 1 prints complete global results
    if (worker_id == 1) {
        // Sort by degree descending
        vector<pair<int,int>> sorted_degrees(local_degree.begin(), local_degree.end());
        sort(sorted_degrees.begin(), sorted_degrees.end(),
            [](const pair<int,int>& a, const pair<int,int>& b) {
                return a.second > b.second;
            });

        cout << "\n" << endl;
        cout << "   DEGREE CENTRALITY RESULTS" << endl;
        cout << "   Total nodes analyzed: " << sorted_degrees.size() << endl;
        cout << "" << endl;
        cout << "Top 20 Most Connected Nodes:" << endl;
        cout << "" << endl;

        int top = min(20, (int)sorted_degrees.size());
        for (int i = 0; i < top; i++) {
            cout << "  Rank " << (i+1) << ": Node " << sorted_degrees[i].first
                 << " | Degree: " << sorted_degrees[i].second << endl;
        }

        // Stats
        int max_deg = sorted_degrees.front().second;
        int min_deg = sorted_degrees.back().second;
        long long total = 0;
        for (auto& p : sorted_degrees) total += p.second;
        double avg = (double)total / sorted_degrees.size();

        cout << "" << endl;
        cout << "Max degree:     " << max_deg << endl;
        cout << "Min degree:     " << min_deg << endl;
        cout << "Avg degree:     " << avg << endl;
        cout << "" << endl;
    }

    return 0;
}
