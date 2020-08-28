#include <iostream>
#include"umpcrawler.h"

int main(int argc, char** argv) {
    printf("hello world\n");
	//std::string filePath = argv[1];
	std::string filePath = "C:\\Users\\Administrator\\1.rawsnapshot";
	//std::cout << filePath;
	Windows tmp = Windows();
	std::cout<<tmp.LoadFromFile(filePath);            
    return 0;
}
