/* msg.x: Remote message printing protocol */ 
 
struct Data {
	int n_reqs;
	int n_hits;
};
program MESSAGEPROG  
{  
    version MESSAGEVERS  
    {  
        string HANDLEREQUEST(string) = 1;  
		Data REPORTSTAT(void) = 2;  
    } = 1;  
} = 99;  
