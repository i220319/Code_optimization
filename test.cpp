#include <iostream>
#include <vector>
using namespace std;

class A{
    char *s;
public:
    A(){

    }
    A(int len){
        s = new char[len];
        s[0]='s';
        s[1]='b';
    }
    char* getS(){
        return s;
    }
};
char *s;
void test(){
    A* a =new A(10000000);
    s = a->getS();
}

int main(){
    int i=1;
    while(1){
        test();
        cout<<s<<i++<<endl;
    }
    return 0;
}
