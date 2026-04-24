#!/bin/bash

# ================================================
#   HYBRID CLOUD BFS - FULL AUTOMATED PIPELINE
# ================================================

GCP_IP="100.98.11.34"
GCP_USER="razzfauzdar"
PROJECT_DIR="~/hybrid_sna"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}"
echo " "
echo "   HYBRID CLOUD SOCIAL NETWORK ANALYZER"
echo "   Baadal (IIT Delhi) + GCP (us-central1)"
echo " "
echo -e "${NC}"

# ── STEP 1: Check input ──
if [ -z "$1" ]; then
    echo -e "${RED}Usage: ./run_hybrid.sh <graph_file>${NC}"
    echo "Example: ./run_hybrid.sh medium_graph.txt"
    exit 1
fi

GRAPH_FILE=$1
if [ ! -f "$GRAPH_FILE" ]; then
    echo -e "${RED}Error: File '$GRAPH_FILE' not found!${NC}"
    exit 1
fi

echo -e "${YELLOW}[1/5] Analyzing graph: $GRAPH_FILE${NC}"

# ── STEP 2: Run controller to get split percentage ──
CONTROLLER_OUTPUT=$(python3 controller.py $GRAPH_FILE 2>&1)
echo "$CONTROLLER_OUTPUT"

SPLIT=$(echo "$CONTROLLER_OUTPUT" | grep "split" | grep -oP '\d+(?=:)')

if [ -z "$SPLIT" ]; then
    echo -e "${YELLOW}      Graph fits in local memory. No bursting needed.${NC}"
    echo -e "${YELLOW}      For demo purposes, forcing 70:30 split...${NC}"
    SPLIT=70
else
    echo -e "${GREEN}      Memory exhaustion predicted!${NC}"
    echo -e "${GREEN}      Triggering hybrid cloud burst: ${SPLIT}:$((100-SPLIT)) split${NC}"
fi

echo ""
echo -e "${YELLOW}[2/5] Partitioning graph ($SPLIT% Baadal, $((100-SPLIT))% GCP)...${NC}"

# ── STEP 3: Run ingest to partition ──
./ingest $GRAPH_FILE $SPLIT
echo -e "${GREEN}      Partitioning complete!${NC}"

# ── STEP 4: Copy GCP partition to GCP VM ──
echo ""
echo -e "${YELLOW}[3/5] Copying GCP partition to GCP VM...${NC}"
scp -o StrictHostKeyChecking=no gcp_partition.txt $GCP_USER@$GCP_IP:$PROJECT_DIR/
echo -e "${GREEN}      Partition copied!${NC}"

# ── STEP 5: Flush Redis ──
echo ""
echo -e "${YELLOW}[4/5] Starting distributed BFS...${NC}"
redis-cli -h $GCP_IP FLUSHALL > /dev/null
echo -e "${GREEN}      Redis flushed!${NC}"

# ── STEP 6: Start Worker 1 on Baadal in background first ──
echo -e "${GREEN}      Starting Worker 1 on Baadal...${NC}"
./worker 1 baadal_partition.txt &
WORKER1_PID=$!

# Wait for Worker 1 to flush Redis and register
sleep 5

# ── STEP 7: Start Worker 2 on GCP ──
echo -e "${GREEN}      Starting Worker 2 on GCP...${NC}"
ssh -o StrictHostKeyChecking=no $GCP_USER@$GCP_IP \
    "cd $PROJECT_DIR && nohup ./worker 2 gcp_partition.txt > /tmp/worker2.log 2>&1 &"
echo -e "${GREEN}      Worker 2 started!${NC}"

# ── STEP 8: Wait for Worker 1 to finish ──
echo ""
echo " "
echo "   WORKER 1 (BAADAL) OUTPUT:"
echo " "
wait $WORKER1_PID

# ── STEP 9: Show GCP Worker 2 output ──
echo ""
echo ""
echo "   WORKER 2 (GCP) OUTPUT:"
echo ""
# Wait a moment for Worker 2 to finish writing log
sleep 10
ssh -o StrictHostKeyChecking=no $GCP_USER@$GCP_IP "cat /tmp/worker2.log"
echo ""

echo ""
echo -e "${GREEN} Hybrid BFS Pipeline Complete!${NC}"
