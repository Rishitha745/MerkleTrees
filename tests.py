import subprocess
import csv
import time
import os

# -------------------------------------------------------------
# CONFIGURATIONS TO TEST
# -------------------------------------------------------------

DEPTHS       = [12, 16, 20, 24]
THREADS      = [2, 4, 8, 16, 32, 64]
BATCH_SIZES  = [1024, 2048, 4096]
TOTAL_OPS    = [50000, 100000, 500000, 1000000]   # you can add more

BINARY = "./bench.out"    # <-- your compiled C++ program
SUMMARY_FILE = "all_results.csv"


# -------------------------------------------------------------
# Helper: Run one configuration
# -------------------------------------------------------------
def run_one(depth, batch, threads, ops):
    print(f"Running: depth={depth}, batch={batch}, threads={threads}, ops={ops}")

    # Create stdin input exactly how your C++ expects:
    input_data = f"{depth}\n{batch}\n{threads}\n{ops}\n"

    # Run the program
    result = subprocess.run(
        [BINARY],
        input=input_data.encode(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    # Wait a moment (files may take a moment to flush)
    time.sleep(0.2)

    # Check if summary file exists
    if not os.path.exists("summary_metrics.csv"):
        print("ERROR: summary_metrics.csv not produced!")
        return None

    # Read the summary metrics written by this run
    with open("summary_metrics.csv", "r") as f:
        reader = csv.DictReader(f)
        rows = list(reader)

    if len(rows) == 0:
        print("ERROR: summary_metrics.csv empty!")
        return None

    return rows[0]   # the single row


# -------------------------------------------------------------
# MAIN LOOP — runs all combinations
# -------------------------------------------------------------
all_results = []
count = 0

for d in DEPTHS:
    for b in BATCH_SIZES:
        for t in THREADS:
            for o in TOTAL_OPS:
                row = run_one(d, b, t, o)
                if row:
                    all_results.append(row)
                count += 1

print(f"\nCompleted {count} runs.")
print(f"Collected {len(all_results)} result rows.\n")


# -------------------------------------------------------------
# WRITE MASTER CSV
# -------------------------------------------------------------
if all_results:
    with open(SUMMARY_FILE, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=all_results[0].keys())
        writer.writeheader()
        writer.writerows(all_results)

    print(f"Master CSV written: {SUMMARY_FILE}")
else:
    print("No results collected!")
