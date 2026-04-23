#include <iostream>
#include <fstream>
#include <string>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./ingest <dataset_file.txt> <baadal_capacity_percentage>" << endl;
        return 1;
    }

    string filename = argv[1];
    int baadal_percentage = stoi(argv[2]);

    ifstream infile(filename);
    ofstream baadal_out("baadal_partition.txt");
    ofstream gcp_out("gcp_partition.txt");

    if (!infile.is_open()) {
        cerr << "Error: Could not open dataset file!" << endl;
        return 1;
    }

    int u, v;

    // Read the graph edge by edge in O(N) time
    while (infile >> u >> v) {
        
        // --- THIS IS THE NEW HASHING LOGIC ---
        // A simple bit-mixing multiplicative hash using a large prime
        unsigned int mixed_hash = (unsigned int)u * 2654435761; 
        
        // Now take the modulo 100 for the percentage probability
        int hash_val = mixed_hash % 100;
        // -------------------------------------

        if (hash_val < baadal_percentage) {
            // Belongs to Baadal VM
            baadal_out << u << " " << v << "\n";
        } else {
            // Overflows to GCP
            gcp_out << u << " " << v << "\n";
        }
    }

    infile.close();
    baadal_out.close();
    gcp_out.close();

    cout << "Partitioning complete. Graph split with " << baadal_percentage 
         << "% weight to Baadal." << endl;
         
    return 0;
}
