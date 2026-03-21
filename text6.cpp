



vector<int> quchong(vector<int>& nums){
    unordered_set<int> s(nums.begin(), nums.end());
    return vector<int>(s.begin(),s.end());
}