# Parallelising Merkle Tree Updates
This repository is part of Mini Project (Jan-Apr 2025) on the Topic of Merkle Trees. 

- **ParallelUpdates** is a **C++** program that implements a parallelized Sparse Merkle Tree.
- It supports secure concurrent updates and reads using **OpenSSL** (for cryptographic hashing) and **multithreading** (using pthread).
- The project ensures correctness by comparing the results of parallel execution with serial execution for verification.



### Main Code file : 
```
ParallelUpdates.cpp
```

### Installing Dependencies :

(Mac OS):
```
brew install openssl
```
(Ubuntu): 
```
sudo apt update
sudo apt install libssl-dev
```

### Compilation command : 

(Mac OS): 
```
g++ ParallelUpdates.cpp -o ParallelUpdates.out -I$(brew --prefix openssl)/include -L$(brew --prefix openssl)/lib -lssl -lcrypto
```
(Ubuntu):
```
g++ ParallelUpdates.cpp -o ParallelUpdates.out -lssl -lcrypto -pthread
```
        
### Execution command :  
```
./ParallelUpdates.out
```

You will be promted to enter:
```
Enter tree depth, read percentage, number of threads, and total operations:
```
Example Input: 
```
20 30 8 10000
```
