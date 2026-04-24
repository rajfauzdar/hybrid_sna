import sys
import os
import psutil
import subprocess

# --- Heuristic Constants ---
# C++ unordered_map overhead per vertex is roughly 32 bytes
# C++ vector overhead per edge (storing a 32-bit int) is 4 bytes
# We use a safety multiplier of 3.0 because vectors double their capacity dynamically
SAFETY_MULTIPLIER = 3.0

def calculate_graph_memory(file_path):
    print(f"Scanning dataset: {file_path}...")
    
    unique_vertices = set()
    edge_count = 0
    
    # O(N) scan to find V and E
    with open(file_path, 'r') as f:
        for line in f:
            if line.strip():
                u, v = line.split()
                unique_vertices.add(u)
                unique_vertices.add(v)
                edge_count += 1
                
    v_count = len(unique_vertices)
    
    # Calculate bytes required
    map_memory = v_count * 32
    vector_memory = edge_count * 4
    total_estimated_bytes = (map_memory + vector_memory) * SAFETY_MULTIPLIER
    
    # Convert to Megabytes
    total_mb = total_estimated_bytes / (1024 * 1024)
    print(f"Graph Profile: {v_count} Vertices, {edge_count} Edges.")
    print(f"Estimated RAM requirement: {total_mb:.2f} MB")
    
    return total_estimated_bytes

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 controller.py <dataset_file.txt>")
        sys.exit(1)

    file_path = sys.argv[1]
    
    # 1. Calculate required memory
    required_bytes = calculate_graph_memory(file_path)
    
    # 2. Check Baadal's available RAM
    mem_info = psutil.virtual_memory()
    available_bytes = mem_info.available
    
    # Let's reserve 20% of Baadal's RAM for the OS and Redis to be safe
    safe_baadal_bytes = 8 * 1024 * 1024  # simulate 8MB limit
    
    print(f"Baadal VM Safe Available RAM: {safe_baadal_bytes / (1024*1024):.2f} MB")
    
    # 3. Calculate Split Percentage
    if required_bytes <= safe_baadal_bytes:
        print("Status: LOCAL EXECUTION SAFE. No cloud bursting required.")
        baadal_percentage = 100
    else:
        print("Status: MEMORY EXHAUSTION PREDICTED. Triggering Hybrid Cloud Burst!")
        # Calculate exactly what percentage Baadal can handle
        baadal_percentage = int((safe_baadal_bytes / required_bytes) * 100)
        
    # 4. Trigger the C++ Ingestion Pipeline
    print(f"\nExecuting C++ Partitioner with a {baadal_percentage}:{100-baadal_percentage} split...")
    
    try:
        subprocess.run(["./ingest", file_path, str(baadal_percentage)], check=True)
    except FileNotFoundError:
        print("Error: Could not find the compiled './ingest' executable. Did you compile it?")
        
if __name__ == "__main__":
    main()
