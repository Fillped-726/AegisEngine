

bool Jxecs(Node* root){
    if(root==nullptr){
        return true;
    }
    deque<Node*> q;
    q.push_back(root);
    while(!q.empty()){
        int size=q.size();
        Node* prev=nullptr;
        for(int i=0;i<size;i++){
            Node* node =q.front();
            q.pop_front();
            if(i%2==0){
                prev=node;
                if(node->left){
                    q.push_front(node->left);
                }
                if(node->right){
                    q.push_back(node->right);
                }
            }
            else{
                if(prev->val!=node->val){
                    return false;
                }
                if(node->right){
                    q.push_front(node->right);
                }
                if(node->left){
                    q.push_back(node->left);
                }
            }
        }
    }
    return true;
}