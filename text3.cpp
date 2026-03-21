#include<stack>
#include<string>
using namespace std;




bool is_hefa(string s){
    stack<char> st;
    for(auto c:s){
        if(c=='(' || c=='[' || c=='{'){
            st.push(c);
        }
        else if(s.empty()){
            return false;
        }
        else if(c==')' && st.top()=='('){
            st.pop();
        }
        else if(c==']' && st.top()=='['){
            st.pop();
        }
        else if(c=='}' && st.top()=='{'){
            st.pop();
        }
        else{
            return false;
        }
    }
    return st.empty();
}