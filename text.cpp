#include <vector>
#include <iostream>
#include <algorithm>
using namespace std;

int main()
{
    int t;
    cin >> t;
    for (int i = 0; i < t; i++)
    {
        int n;
        cin >> n;
        string s;
        cin >> s;
        string temp;
        for (int i = 0; i < s.size(); i++)
        {
            if (s[i] == ' ')
            {
                continue;
            }
            temp.push_back(s[i]);
        }
        sort(temp.begin(), temp.end());
        cout << temp << endl;
    }
}