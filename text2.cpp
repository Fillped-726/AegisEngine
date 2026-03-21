



bool is_huan(Node* root){
    
    Node* slow=root;
    Node* fast=root;
    while(fast!=nullptr && fast->next!=nullptr){
        slow=slow->next;
        fast=fast->next->next;
        if(slow==fast){
            return true;
        }
    }
    return false;
}