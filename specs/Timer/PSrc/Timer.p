/*****************************************************************************************
The timer state machine models the non-deterministic behavior of an OS timer
******************************************************************************************/
machine Timer
{
    // user of the timer
  var client: machine;
  start state Init {
    entry (_client : machine){
      client = _client;
      goto WaitForTimerRequests;
    }
  }

  state WaitForTimerRequests {
    on eStartTimer goto TimerStarted;
    ignore eCancelTimer, eDelayedTimeOut;
  }

  state TimerStarted {
    defer eStartTimer;
    entry {
      if($)
      {
        send client, eTimeOut;
        goto WaitForTimerRequests;
      }
      else
      {
        send this, eDelayedTimeOut;
      }
    }
    on eDelayedTimeOut goto TimerStarted;
    on eCancelTimer goto WaitForTimerRequests;
  }
}

/************************************************
Events used to interact with the timer machine
************************************************/
event eStartTimer;
event eCancelTimer;
event eTimeOut;
event eDelayedTimeOut;
/************************************************
Functions or API's to interact with the OS Timer
*************************************************/
// create timer
fun CreateTimer(client: machine) : Timer
{
  return new Timer(client);
}

// start timer
fun StartTimer(timer: Timer)
{
  send timer, eStartTimer;
}

// cancel timer
fun CancelTimer(timer: Timer)
{
  send timer, eCancelTimer;
}