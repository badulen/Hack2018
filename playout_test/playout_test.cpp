#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>

void pause_us(uint32_t us)
{
    using namespace std::this_thread;
    using namespace std::chrono;
    sleep_for(microseconds(us));
}

int main ( int argc, char ** argv)
{
    using std::cout;
    using std::endl;
    using std::ofstream;
    using std::ifstream;
    using std::string;
    using std::ios;

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
    while (infile.get(c))
    {
        if (infile.eof()) {
            break;
        }
        outfile << c;
    }

    infile.close();
    outfile.close();
    return 0;
}
