






Node* reverse(Node* head){
    Node* pre=nullptr;
    Node* curr=head;
    while(curr!=nullptr){
        Node* next=curr->next;
        curr->next=pre;
        pre=curr;
        curr=next;
    }
    return pre;
}

Node* fanzhuan(Node* head,int size,int m,int n){
    if(head==nullptr || m>n || m<1 || n>size){
        return head;
    }
    Node dummy(-1);
    dummy.next=head;
    Node* pre=&dummy;
    for(int i=0;i<m-1;i++){
        pre=pre->next;
    }
    Node* start=pre->next;
    Node* end=start;
    pre->next=nullptr;
    for(int i=0;i<n-m;i++){
        end=end->next;
    }
    Node* next=end->next;
    end->next=nullptr;
    pre->next=reverse(start);
    start->next=next;
    return dummy.next;
}