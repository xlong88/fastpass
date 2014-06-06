# This script reads in several CSV files of log data from a linux test
# and computes properties of the fairness over intervals of a specified size.
# Output not guaranteed to be reasonable for interval sizes smaller
# or equal to the measured intervals

import csv
import sys

# parse command line arguments
args = sys.argv
if len(args) < 3:
    print 'Usage: python %s bin_size (ns) input_files' % str(args[0])
    exit()
bin_size = int(args[1])

NUM_FILES = len(args) - 2
input_files = []
for i in range(NUM_FILES):
    input_files.append(str(args[i + 2]))

# open and read files
readers = []
next_rows = []
for f in input_files:
    reader = csv.reader(open(f, 'r'))
    readers.append(reader)
    header = reader.next()

    row = reader.next()
    next_rows.append(row)

sys.stdout.write("start_time,end_time,bytes,active_flows\n")
 
min_start = int(next_rows[0][0])
for row in next_rows:
    start_t = int(row[0])
    if start_t < min_start:
        min_start = start_t
bin_start = min_start
bin_end = bin_start + bin_size

flow_bytes = []
for i in range(NUM_FILES):
    flow_bytes.append(0)

BYTES_INDEX = 6

# we want to distinguish between when we are adding flows
# and when we are removing flows
max_active_flows = 0
mode = "adding"

num_done = 0
finished = [False, False, False, False, False]
while num_done < NUM_FILES:
    # process each file
    for i in range(NUM_FILES):
        # skip this file if we've already read all of it
        if finished[i]:
            continue

        # process all bins that overlap this bin
        row = next_rows[i]
        while int(row[0]) < bin_end:
            if int(row[1]) <= bin_end:
                flow_bytes[i] += int(row[BYTES_INDEX])
                try:
                    row = readers[i].next()
                except StopIteration:
                    num_done += 1
                    finished[i] = True
                    break
            else:
                partial_bytes = int(row[BYTES_INDEX]) * (bin_end - int(row[0])) / (int(row[1]) - int(row[0]))
                flow_bytes[i] += partial_bytes
                row[BYTES_INDEX] = int(row[BYTES_INDEX]) - partial_bytes
                row[0] = bin_end
        next_rows[i] = row

    # computed flow_bytes for all files, now output bytes
    active_flows = 0
    for i in range(NUM_FILES):
        if flow_bytes[i] > 0:
            active_flows += 1

    if active_flows > max_active_flows:
        max_active_flows = active_flows
    if max_active_flows == 5:
        mode = "subtracting"

    # write out bin data
    for i in range(NUM_FILES):
        if flow_bytes[i] > 0:
            flow_str = str(active_flows) + '_' + mode
            s = str(bin_start) + ', ' + str(bin_end) + ', ' + str(flow_bytes[i]) + ', ' + flow_str
            s += '\n'
            sys.stdout.write(s)

    # start a new bin
    for i in range(NUM_FILES):
        flow_bytes[i] = 0
    bin_start = bin_end
    bin_end += bin_size
