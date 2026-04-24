#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <queue>
#include <unistd.h> // For usleep (pause function)
#include <hiredis/hiredis.h>
#include <unordered_map>

using namespace std;

// --- Global State ---
unordered_map<int, vector<int>> adj_list;
unordered_map<int, int> distances;
const int BAADAL_PERCENTAGE = 70; 
const int INF = 99999999;

// Worker Config
int worker_id;
string my_queue, other_queue;

// --- Hashing & Routing ---
bool is_local(int vertex_id) {
    unsigned int mixed_hash = (unsigned int)vertex_id * 2654435761;
    int percentage = mixed_hash % 100;
    
    // If Worker 1 (Baadal), we own < 70. If Worker 2 (GCP), we own >= 70.
    if (worker_id == 1) return percentage < BAADAL_PERCENTAGE;
    else return percentage >= BAADAL_PERCENTAGE;
}

// --- Partition Loader ---
bool load_partition(const string& filename) {
    ifstream infile(filename);
    if (!infile.is_open()) return false;
    int u, v;
    while (infile >> u >> v) {
        adj_list[u].push_back(v);
        if (distances.find(u) == distances.end()) distances[u] = INF;
        if (distances.find(v) == distances.end()) distances[v] = INF;
    }
    infile.close();
    return true;
}

// --- The Bulk Synchronous Parallel (BSP) Loop ---
void run_bsp_bfs(redisContext* redis, int source_vertex) {
    queue<int> current_q;
    queue<int> next_q;
    unordered_map<int, int> outgoing_best;
    if (is_local(source_vertex)) {
        distances[source_vertex] = 0;
        current_q.push(source_vertex);
        cout << "[Superstep 0] Source initialized locally." << endl;
    }

    int superstep = 0;

    while (true) {
        // -----------------------------------------------------
        // PHASE 1: COMPUTE & COMMUNICATE
        // -----------------------------------------------------
        int messages_sent = 0;
        while (!current_q.empty()) {
            int curr = current_q.front();
            current_q.pop();

            for (int neighbor : adj_list[curr]) {
                int new_dist = distances[curr] + 1;
                if (is_local(neighbor)) {
                    if (new_dist < distances[neighbor]) {
                        distances[neighbor] = new_dist;
                        next_q.push(neighbor);
                    }
                } 
else {
    if (outgoing_best.find(neighbor) == outgoing_best.end() ||
        new_dist < outgoing_best[neighbor]) {
        outgoing_best[neighbor] = new_dist;
    }
}
            }
        }
messages_sent = outgoing_best.size();
if (!outgoing_best.empty()) {
    // Build all message strings first and store stably
    vector<string> msg_strs;
    msg_strs.reserve(outgoing_best.size());
    for (auto& pair : outgoing_best) {
        msg_strs.push_back(to_string(pair.first) + ":" + to_string(pair.second));
    }
    
    // Now build argv from stable strings
    vector<const char*> argv;
    vector<size_t> argvlen;
    argv.push_back("LPUSH");
    argvlen.push_back(5);
    argv.push_back(other_queue.c_str());
    argvlen.push_back(other_queue.size());
    
    for (auto& msg : msg_strs) {
        argv.push_back(msg.c_str());
        argvlen.push_back(msg.size());
    }
    
    redisReply* r = (redisReply*)redisCommandArgv(redis, argv.size(), argv.data(), argvlen.data());
    if (r) freeReplyObject(r);
    outgoing_best.clear();
}
        // -----------------------------------------------------
        // PHASE 2: TELL OTHER WORKER HOW MANY MESSAGES WE SENT
        // -----------------------------------------------------
        string signal_key = "signal_" + to_string(superstep) + "_" + to_string(worker_id);
        redisCommand(redis, "SET %s %d", signal_key.c_str(), messages_sent);
        redisCommand(redis, "EXPIRE %s 300", signal_key.c_str());

        cout << "[Superstep " << superstep << "] Sent " << messages_sent << " messages. Waiting..." << endl;

        // -----------------------------------------------------
        // PHASE 3: WAIT FOR OTHER WORKER TO FINISH SENDING
        // -----------------------------------------------------
        int other_id = (worker_id == 1) ? 2 : 1;
        string other_signal = "signal_" + to_string(superstep) + "_" + to_string(other_id);

        while (true) {
            redisReply* r = (redisReply*)redisCommand(redis, "GET %s", other_signal.c_str());
            if (r && r->str) {
                freeReplyObject(r);
                break;
            }
            if (r) freeReplyObject(r);
            usleep(50000); // 50ms
        }

        // -----------------------------------------------------
        // PHASE 4: READ ALL INCOMING MESSAGES
        // -----------------------------------------------------

int received = 0;

// FETCH ENTIRE QUEUE IN ONE SHOT - single network call!
redisReply* all_msgs = (redisReply*)redisCommand(redis,
    "LRANGE %s 0 -1", my_queue.c_str());

if (all_msgs && all_msgs->type == REDIS_REPLY_ARRAY && all_msgs->elements > 0) {
    for (size_t i = 0; i < all_msgs->elements; i++) {
        if (!all_msgs->element[i]->str || all_msgs->element[i]->len == 0) continue;
        string msg = all_msgs->element[i]->str;
        if (msg.find(':') == string::npos) continue;
        int delim = msg.find(':');
        int node = stoi(msg.substr(0, delim));
        int dist = stoi(msg.substr(delim + 1));
        if (dist < distances[node]) {
            distances[node] = dist;
            next_q.push(node);
        }
        received++;
    }
    // Clear entire queue in one shot
    redisReply* del_r = (redisReply*)redisCommand(redis, "DEL %s", my_queue.c_str());
    if (del_r) freeReplyObject(del_r);
}
if (all_msgs) freeReplyObject(all_msgs);
        cout << "[Superstep " << superstep << "] Received " << received << " messages." << endl;
        // -----------------------------------------------------
        // PHASE 5: TERMINATION CHECK
        // -----------------------------------------------------
        // Tell other worker if we are active
        int active = (!next_q.empty()) ? 1 : 0;
        string active_key = "active2_" + to_string(superstep) + "_" + to_string(worker_id);
        redisCommand(redis, "SET %s %d", active_key.c_str(), active);
        redisCommand(redis, "EXPIRE %s 300", active_key.c_str());

        // Wait for other worker's active status
        string other_active_key = "active2_" + to_string(superstep) + "_" + to_string(other_id);
        while (true) {
            redisReply* r = (redisReply*)redisCommand(redis, "GET %s", other_active_key.c_str());
            if (r && r->str) {
                int other_active = atoi(r->str);
                freeReplyObject(r);
                if (active == 0 && other_active == 0) {
                    cout << "Global BFS Termination reached at Superstep " << superstep << "!" << endl;

// ─── RESULT COLLECTION ───
// Worker 2 (GCP) sends all its distances to Baadal via Redis
if (worker_id == 2) {
    cout << "Sending distance results to Baadal..." << endl;
    
    vector<string> result_strs;
    vector<const char*> argv;
    vector<size_t> argvlen;
    
    argv.push_back("LPUSH");
    argvlen.push_back(5);
    argv.push_back("result_queue");
    argvlen.push_back(12);
    
    for (auto& pair : distances) {
        if (pair.second != INF) {
            result_strs.push_back(to_string(pair.first) + ":" + to_string(pair.second));
        }
    }
    for (auto& msg : result_strs) {
        argv.push_back(msg.c_str());
        argvlen.push_back(msg.size());
    }
    redisReply* r = (redisReply*)redisCommandArgv(redis, argv.size(), argv.data(), argvlen.data());
    if (r) freeReplyObject(r);
    
    // Signal that results are sent
    redisCommand(redis, "SET result_ready 1");
    cout << "Results sent!" << endl;
}

// Worker 1 (Baadal) waits for GCP results then merges and prints
if (worker_id == 1) {
    cout << "Waiting for GCP results..." << endl;
    
    // Wait for GCP to signal ready
    while (true) {
        redisReply* r = (redisReply*)redisCommand(redis, "GET result_ready");
        if (r && r->str && atoi(r->str) == 1) {
            freeReplyObject(r);
            break;
        }
        if (r) freeReplyObject(r);
        usleep(100000);
    }
    
    // Fetch all GCP distances in one shot
    redisReply* all_results = (redisReply*)redisCommand(redis, "LRANGE result_queue 0 -1");
    
    // Merge GCP distances into local distances map
    if (all_results && all_results->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < all_results->elements; i++) {
            if (!all_results->element[i]->str) continue;
            string msg = all_results->element[i]->str;
            int delim = msg.find(':');
            if (delim == (int)string::npos) continue;
            int node = stoi(msg.substr(0, delim));
            int dist = stoi(msg.substr(delim + 1));
            distances[node] = dist; // merge into Baadal's map
        }
        freeReplyObject(all_results);
    }
    
    // ─── PRINT COMPLETE GLOBAL RESULTS ───
    cout << "\n" << endl;
    cout << "   COMPLETE GLOBAL BFS RESULTS" << endl;
    cout << "   Source Node: " << source_vertex << endl;
    cout << "\n" << endl;
    
    int reachable = 0, unreachable = 0, max_dist = 0;
    map<int,int> dist_histogram; // distance → count of nodes
    
    for (auto& pair : distances) {
        if (pair.second != INF) {
            reachable++;
            max_dist = max(max_dist, pair.second);
            dist_histogram[pair.second]++;
        } else {
            unreachable++;
        }
    }
    
    cout << "Total vertices seen:          " << distances.size() << endl;
    cout << "Reachable vertices:           " << reachable << endl;
    cout << "Unreachable vertices:         " << unreachable << endl;
    cout << "max depth:   " << max_dist << endl;
    cout << "\nBFS Level Distribution:" << endl;
    for (auto& pair : dist_histogram) {
        cout << "  Distance " << pair.first << ": " << pair.second << " nodes" << endl;
    }
    
    // Sample of 15 closest nodes
    cout << "\nSample distances from source " << source_vertex << ":" << endl;
    int count = 0;
    for (auto& pair : distances) {
        if (pair.second != INF && pair.second > 0 && count < 15) {
            cout << "  Node " << pair.first << " → distance " << pair.second << endl;
            count++;
        }
    }
    cout << "\n" << endl;
}
return;
                }
                break;
            }
            if (r) freeReplyObject(r);
            usleep(50000);
        }

        swap(current_q, next_q);
        superstep++;
    }
}

int main(int argc, char* argv[]) {
    // Requires Worker ID and Partition File
    if (argc != 3) {
        cerr << "Usage: ./worker <worker_id_1_or_2> <partition_file.txt>" << endl;
        return 1;
    }

    worker_id = stoi(argv[1]);
    
    // Assign Redis queues based on identity
    if (worker_id == 1) {
        my_queue = "queue_baadal";
        other_queue = "queue_gcp";
    } else {
        my_queue = "queue_gcp";
        other_queue = "queue_baadal";
    }

    if (!load_partition(argv[2])) return 1;

    redisContext *redis = redisConnect("100.98.11.34", 6379);
    if (redis == NULL || redis->err) return 1;
    
    // Clear old state from Redis to ensure a clean run
    if (worker_id == 1) {
    redisCommand(redis, "FLUSHALL");
    cout << "Redis state cleared by Master Node." << endl;
    usleep(300000); // let Worker 2 notice the flush
}

// READY BARRIER — both workers meet here before BFS starts
cout << "Registering as ready..." << endl;
redisCommand(redis, "INCR ready_count");

while (true) {
    redisReply* r = (redisReply*)redisCommand(redis, "GET ready_count");
    if (r && r->str && atoi(r->str) >= 2) {
        freeReplyObject(r);
        break;
    }
    if (r) freeReplyObject(r);
    usleep(200000);
}
cout << "Both workers ready! Starting BFS..." << endl;

// Start BFS from Source Node 1
run_bsp_bfs(redis, 1);
    redisFree(redis);
    return 0;
}
