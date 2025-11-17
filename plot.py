import matplotlib.pyplot as plt

# ============================================
# RAW DATA FROM YOUR EXPERIMENT
# ============================================

threads = [2,4,8,16,32,64]

# -------- Depth 12 --------

live_12_1024 =  [263565390.54, 256955208.44, 253616171.24, 251981618.15, 250896662.06, 252126218.54]
angela_12_1024 = [285098957.62, 269365228.76, 261623907.16, 257233867.91, 254396477.10, 255443419.44]
serial_12_1024 = [317804651.61, 299605656.02, 290522128.86, 285094096.65, 281344473.75, 282369212.55]

live_12_2048 =  [263782826.71, 256632114.55, 253828216.80, 251584149.03, 250995906.00, 252497285.21]
angela_12_2048 = [286242130.31, 269530238.65, 261857210.31, 256967697.02, 254772057.02, 256296788.57]
serial_12_2048 = [319844301.59, 300351421.83, 290994413.19, 284987265.75, 281878867.01, 283636136.75]

live_12_4096 =  [263747291.12, 256195491.70, 253607332.17, 252259285.21, 250568876.01, 250736500.14]
angela_12_4096 = [288004257.87, 270351237.37, 262243075.98, 258536347.68, 255263882.79, 254980233.09]
serial_12_4096 = [321775812.17, 302232144.35, 291869073.89, 287826069.47, 283254251.88, 283122099.38]

# -------- Depth 16 (Only batch=1024 available) --------
threads_16 = [2,4,8, 16, 32, 64]
live_16_1024 =  [269710068.96, 260785874.51, 256498155.51, 254225056.72, 253369588.18, 255064051.74]
angela_16_1024 = [302633178.78, 278548500.24, 266979039.01, 261266065.44, 258255142.42, 260890680.48]
serial_16_1024 = [350492668.91, 320841411.03, 305573967.12, 298660811.33, 294293757.92, 297407040.48]


# =====================================================
# GRAPH 1: Depth 12 – Varying Batch Size
# =====================================================
# Batch sizes
import matplotlib.pyplot as plt
import numpy as np
from scipy.interpolate import make_interp_spline

# -------------------------------------------
# Original data points
# -------------------------------------------

batches = np.array([1024, 2048, 4096])

# Thread index for threads = 32
idx_32 = 4

live_batches = np.array([
    live_12_1024[idx_32],
    live_12_2048[idx_32],
    live_12_4096[idx_32]
])

angela_batches = np.array([
    angela_12_1024[idx_32],
    angela_12_2048[idx_32],
    angela_12_4096[idx_32]
])

serial_batches = np.array([
    serial_12_1024[idx_32],
    serial_12_2048[idx_32],
    serial_12_4096[idx_32]
])

# # -------------------------------------------
# # Spline interpolation for smooth curves
# # -------------------------------------------
# x_smooth = np.linspace(batches.min(), batches.max(), 200)

# live_smooth   = make_interp_spline(batches, live_batches, k=2)(x_smooth)
# angela_smooth = make_interp_spline(batches, angela_batches, k=2)(x_smooth)
# serial_smooth = make_interp_spline(batches, serial_batches, k=2)(x_smooth)

# -------------------------------------------
# Plotting
# -------------------------------------------

plt.figure(figsize=(10,6))

plt.plot(batches, live_batches,marker='o',  label="Live Algorithm", linewidth=2)
plt.plot(batches, angela_batches,marker='o',  label="Angela Algorithm", linewidth=2)
plt.plot(batches, serial_batches, marker='o', label="Serial Algorithm", linewidth=2)

# # Mark actual data points
# plt.scatter(batches, live_batches,marker='o',  color='blue')
# plt.scatter(batches, angela_batches,marker='o',  color='orange')
# plt.scatter(batches, serial_batches, marker='o', color='green')

plt.xticks([1024, 2048, 4096])  # ONLY your points
plt.title("Response Time vs Batch Size")
plt.xlabel("Batch Size")
plt.ylabel("Response Time (us)")
# plt.grid(True)
plt.legend()
plt.tight_layout()
plt.show()



# =====================================================
# GRAPH 2: Depth 12 – Varying Threads (1024 batch)
# =====================================================
plt.figure(figsize=(10,6))

plt.plot(threads, live_12_1024, marker='o', label="Live Algorithm")
plt.plot(threads, angela_12_1024, marker='o', label="Angela Algorithm")
plt.plot(threads, serial_12_1024, marker='o', label="Serial Algorithm")

plt.xticks([2,4,8,16,32,64])
plt.title("Response Time vs Threads")
plt.xlabel("Threads")
plt.ylabel("Response Time (us)")
plt.legend()
plt.tight_layout()
plt.show()


# =====================================================
# GRAPH 3: Varying Depth (12 → 16) for Batch=1024
# Using best thread = 32
# =====================================================

# Best thread index = thread 32 is index 4 in your list
idx_32 = threads.index(32)

depths = [12, 16, 20, 24]

live_depth = [live_12_1024[idx_32], live_16_1024[2], 258566291.49, 343309698.98]      # thread 32 ≈ thread 8 for depth16
angela_depth = [angela_12_1024[idx_32], angela_16_1024[2],270761099.86, 445400426.59 ]
serial_depth = [serial_12_1024[idx_32], serial_16_1024[2], 324087344.05, 594776287.85 ]

plt.figure(figsize=(10,6))

plt.plot(depths, live_depth, marker='o', label="Live Algorithm")
plt.plot(depths, angela_depth, marker='o', label="Angela Algorithm")
plt.plot(depths, serial_depth, marker='o', label="Serial Algorithm")

plt.xticks([12, 16, 20, 24])
plt.title("Response Time vs Depth")
plt.xlabel("Depth")
plt.ylabel("Response Time (us)")
plt.legend()
plt.tight_layout()
plt.show()
