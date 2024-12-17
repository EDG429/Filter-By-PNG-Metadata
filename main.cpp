#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <winsock2.h>
#include <windows.h>
#include <zlib.h>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <future>
#include <functional>

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

public:
    ThreadPool(size_t threads) : stop(false) {
        for(size_t i = 0; i < threads; ++i)
            workers.emplace_back(
                [this] {
                    for (;;) {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                            if(this->stop && this->tasks.empty())
                                return;
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                        task();
                    }
                }
            );
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for(std::thread &worker: workers)
            worker.join();
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>
    {
        using return_type = typename std::result_of<F(Args...)>::type;

        auto task = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace([task](){ (*task)(); });
        }
        condition.notify_one();
        return res;
    }
};

std::string readPngMetadata(const std::string& fileName);

std::mutex mtx;
std::condition_variable cv;
bool allProcessed = false;

void processFile(const std::string& folderPath, const std::string& fileName, std::unordered_map<std::string, std::string>& myDictionary) {
    std::string fullPath = folderPath + "\\" + fileName;
    std::string title = fileName.substr(0, fileName.length() - 4);
    std::string metadata = readPngMetadata(fullPath);
    
    {
        std::lock_guard<std::mutex> lock(mtx);
        myDictionary[title] = metadata;
    }
}

// Directory authenticator
bool DirectoryExists(const char* dirName) {
    DWORD f = GetFileAttributesA(dirName);
    return f != INVALID_FILE_ATTRIBUTES && (f & FILE_ATTRIBUTE_DIRECTORY);
}

// Function to count .png files in the given directory
int countPngFiles(const char* dirName, int& pngCount) {
    WIN32_FIND_DATAA findFileData;
    HANDLE hFind;

    std::string searchPath = dirName;
    searchPath += "\\*.*";
    
    hFind = FindFirstFileA(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::cerr << "Could not find the first file in the directory." << std::endl;
        return 0;
    }

    do {
        if (strcmp(findFileData.cFileName, ".") != 0 && strcmp(findFileData.cFileName, "..") != 0) {
            size_t len = strlen(findFileData.cFileName);
            if (len > 4 && 
                findFileData.cFileName[len - 4] == '.' &&
                findFileData.cFileName[len - 3] == 'p' &&
                findFileData.cFileName[len - 2] == 'n' &&
                findFileData.cFileName[len - 1] == 'g') {
                pngCount++;
            }
        }
    } while (FindNextFileA(hFind, &findFileData) != 0);

    FindClose(hFind);

    return pngCount;
}

// Function to create an empty dictionary
std::unordered_map<std::string, std::string> createEmptyDictionary(int& pngCount) {
    std::unordered_map<std::string, std::string> dictionary;
    return dictionary;
}

// Function that split the words to search
std::vector<std::string> splitWordsToSearch(const std::string& wordsToSearch) {
    std::vector<std::string> splitWords;
    std::istringstream iss(wordsToSearch);
    std::string word;

    // Split the string by comma
    while (std::getline(iss, word, ',')) {
        // Trim leading and trailing spaces from each word
        size_t start = word.find_first_not_of(" ");
        size_t end = word.find_last_not_of(" ");
        if(start != std::string::npos && end != std::string::npos) {
            splitWords.push_back(word.substr(start, end - start + 1));
        } else {
            // If the word is only spaces or empty, we might want to handle it
            splitWords.push_back("");
        }
    }

    return splitWords;
}

// Helper function to read metadata from PNG chunks, focusing only on tEXt chunks with buffered reading
std::string readPngMetadata(const std::string& fileName) {
    std::ifstream file(fileName, std::ios::binary | std::ios::in);
    if (!file) return ""; // Error opening file

    std::string metadata;
    const size_t BUFFER_SIZE = 65536; // 64KB buffer
    char buffer[BUFFER_SIZE];

    // Skip PNG signature
    file.seekg(8, std::ios::beg);

    while (file) {
        uint32_t length, chunkType;
        if (!file.read((char*)&length, 4)) break; // Read length
        length = ntohl(length); // Convert from network to host byte order
        if (!file.read((char*)&chunkType, 4)) break; // Read chunk type
        chunkType = ntohl(chunkType);

        if (chunkType == 0x74455874) { // Check if it's a tEXt chunk
            std::string keyword, text;
            // We only need to read 'length' bytes for tEXt data
            if (length <= BUFFER_SIZE) {
                if (file.read(buffer, length)) {
                    size_t i = 0;
                    while (i < length && buffer[i] != 0) keyword += buffer[i++];
                    i++; // Skip null terminator
                    text = std::string(buffer + i, length - i);
                    metadata += keyword + ": " + text + "\n";
                }
            } else {
                // If the chunk is larger than our buffer, we might need to handle this differently
                // But for simplicity, we'll ignore very large chunks for now
                file.seekg(length, std::ios::cur);
            }
        } else {
            // Skip this chunk since it's not tEXt
            file.seekg(length + 4, std::ios::cur); // Skip chunk data + CRC
        }
    }

    return metadata;
}

// Fill dictionary with metadata, only processing PNG files
std::unordered_map<std::string, std::string> fillDictionaryWithImageMetadata(const std::string& folderPath, std::unordered_map<std::string, std::string> myDictionary, ThreadPool& pool) {
    WIN32_FIND_DATAA findFileData;
    HANDLE hFind;
    std::string searchPath = folderPath + "\\*.png";
    std::vector<std::future<void>> futures; // To keep track of futures

    hFind = FindFirstFileA(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::cerr << "Could not find any PNG files in the directory." << std::endl;
        return myDictionary;
    }

    do {
        std::string fileName = findFileData.cFileName;
        if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            futures.push_back(pool.enqueue([&, fileName]() {
                processFile(folderPath, fileName, myDictionary);
            }));
        }
    } while (FindNextFileA(hFind, &findFileData) != 0);

    FindClose(hFind);

    // Wait for all tasks to complete
    for(auto& f : futures) f.wait();
    return myDictionary;
}

// Function to filter the dictionary based on search terms
void filterDictionary(std::unordered_map<std::string, std::string>& myDictionary, const std::vector<std::string>& wordsToSearch) {
    // Convert all search terms to lowercase for case-insensitive comparison
    std::vector<std::string> lowerCaseSearchWords;
    for (const auto& word : wordsToSearch) {
        std::string lowerWord = word;
        std::transform(lowerWord.begin(), lowerWord.end(), lowerWord.begin(), ::tolower);
        lowerCaseSearchWords.push_back(lowerWord);
    }

    // Iterate through the dictionary and remove entries that don't match all search terms
    for (auto it = myDictionary.begin(); it != myDictionary.end(); ) {
        std::string lowerCaseMetadata = it->second;
        std::transform(lowerCaseMetadata.begin(), lowerCaseMetadata.end(), lowerCaseMetadata.begin(), ::tolower);

        // Check if all words are in the metadata
        bool allWordsFound = std::all_of(lowerCaseSearchWords.begin(), lowerCaseSearchWords.end(),
            [&lowerCaseMetadata](const std::string& word) {
                return lowerCaseMetadata.find(word) != std::string::npos;
            });

        if (!allWordsFound) {
            it = myDictionary.erase(it); // Erase and move to the next item
        } else {
            ++it; // Move to the next item if not erased
        }
    }
}

// Function to move filtered images to a new or existing folder
void moveFilteredImages(const std::unordered_map<std::string, std::string>& myDictionary, const std::string& folderPath) {
    std::string filteredFolder = folderPath + "\\Filtered_Search";

    // Delete existing 'Filtered_Search' directory if it exists
    if (DirectoryExists(filteredFolder.c_str())) {
        RemoveDirectoryA(filteredFolder.c_str()); // Note: this only works if the directory is empty, 
                                                  // for non-empty directories we need to use SHFileOperation
        SHFILEOPSTRUCTA file_op = {
            NULL,
            FO_DELETE,
            filteredFolder.c_str(),
            "",
            FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT,
            false,
            0,
            ""
        };
        SHFileOperationA(&file_op);
    }

    // Create 'Filtered_Search' directory
    if (!CreateDirectoryA(filteredFolder.c_str(), NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            std::cerr << "Failed to create directory: " << GetLastError() << std::endl;
            return;
        }
    }

    // Move filtered images to new directory
    for (const auto& pair : myDictionary) {
        std::string sourcePath = folderPath + "\\" + pair.first + ".png";
        std::string destPath = filteredFolder + "\\" + pair.first + ".png";

        if (!MoveFileA(sourcePath.c_str(), destPath.c_str())) {
            std::cerr << "Failed to move file " << pair.first << ".png: " << GetLastError() << std::endl;
        }
    }
}

int main() {
    int pngCount = 0;
    std::string folderPath;
    

    std::cout << "\n This program serves to filter images of a given folder using the textual PNG metadata of said images.";

    // Loop for valid directory input
    while (true) {
        std::cout << "\nPlease enter a valid folder path: ";
        std::getline(std::cin, folderPath);

        if (DirectoryExists(folderPath.c_str())) break;
        std::cout << "\nInvalid folder path, or the directory does not exist... \nPlease try again.\n";
    }
    std::cout << "\n You have entered a valid folder path.";
    std::cout << "\n There are " << countPngFiles(folderPath.c_str(), pngCount) << " .png files in that folder.";

    // Create threadpool
    ThreadPool pool(std::thread::hardware_concurrency());

    // Create an empty dictionary
    std::unordered_map<std::string, std::string> myDictionary = createEmptyDictionary(pngCount);
    std::cout << "\nA dictionary has been instantiated and has enough space for " << pngCount << " key/value pairs.";

    myDictionary =  fillDictionaryWithImageMetadata(folderPath, myDictionary, pool);
    std::cout << "Finished processing all files." << std::endl;

    // Search for metadata
    std::string wordsToSearch;
    std::cout << "\nPlease enter comma separated tags so the program knows what you are searching for: ";
    std::getline(std::cin, wordsToSearch);

    // Return a vector of the words that were split
    splitWordsToSearch(wordsToSearch);
    std::cout << "\nYou are searching for: " << wordsToSearch;

    std::vector<std::string> searchTerms = splitWordsToSearch(wordsToSearch);

    // Get a filtered dictionary we can use to filter the folder and get the images that have the metadata we want
    filterDictionary(myDictionary, searchTerms);

    moveFilteredImages(myDictionary, folderPath);

    return 0;
}