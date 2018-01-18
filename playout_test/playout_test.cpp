#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <deque>

void pause_ns(const uint32_t & ns)
{
    using namespace std::this_thread;
    using namespace std::chrono;
    sleep_for(nanoseconds(ns));
}

int main ( int argc, char ** argv)
{
    using std::cout;
    using std::endl;
    using std::ofstream;
    using std::ifstream;
    using std::string;
    using std::ios;
    using std::deque;

    deque<char> qbuff;

    // target bitrate in Mbps, b = bit!
    uint32_t target_bitrate_Mbps = 1;
    uint32_t buff_size = 1024; // bytes = 8192 bits
    uint32_t buff_size_bits = buff_size * 8;

    ifstream infile;
    ofstream outfile;
    string ifname("bbc1.ts");
    string ofname("bbc1_copy.ts");

    cout << "Opening input file" << endl;
    infile.open(ifname, ios::binary);

    cout << "Opening output file" << endl;
    outfile.open(ofname, ios::binary);

    cout << "Copying input to output" << endl;
    char c;
    uint32_t i = 0;
    auto start = std::chrono::high_resolution_clock::now();
    auto end = start;
    uint32_t buff_level_bits = 0;

    while (infile.get(c))
    {
        if (infile.eof()) {
            break;
        }

        // push the input into a queue
        qbuff.push_front(c);
        buff_level_bits += 8;

        if (buff_level_bits > buff_size_bits)
        {
            for (unsigned int i = 0; i< buff_level_bits/8; i++)
            {
                pause_ns(1); // The minimum delay seems to be about 60us!

                c = qbuff.back();
                qbuff.pop_back();
                outfile << c;
            }
            buff_level_bits = 0;
        }

//        start = std::chrono::high_resolution_clock::now();

        //
//      nanosleep(&delay, NULL);    // minimum delay also about 60us
//        end = std::chrono::high_resolution_clock::now();
//        std::chrono::duration<double, std::micro> elapsed = end-start;
//        std::cout << "Waited " << elapsed.count() << " us\n";
    }

    infile.close();
    outfile.close();
    return 0;
}
