#include <iostream>
#include <vector>
#include <unordered_set>
#include <algorithm>
using namespace std;

int final(vector<vector<pair<int, int>>> &g, int i, int k, bool first, int prev)
{
    int flag = false;
    for (auto &[v, w] : g[i])
    {
        if (w <= k)
        {
            if (v != prev)
            {
                flag = true;
                return final(g, v, k, false, i);
            }
        }
    }
    if (!flag)
    {
        return i;
    }
}

int main()
{
    int n, m, k;
    cin >> n >> m >> k;
    vector<vector<pair<int, int>>> g(n + 1);
    for (int i = 0; i < m; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            int u, v, w;
            cin >> u >> v >> w;
            g[u].push_back({v, w});
            g[v].push_back({u, w});
        }
    }
    int count = 0;
    unordered_set<int> s;
    for (int i = 1; i <= n; i++)
    {
        int f = final(g, i, k, true, 0);
        if (s.find(f) == s.end())
        {
            s.insert(f);
            count++;
        }
    }
    cout << count << endl;
    vector<int> arr(s.begin(), s.end());
    sort(arr.begin(), arr.end());
    for (auto &x : arr)
    {
        cout << x << " ";
    }
}