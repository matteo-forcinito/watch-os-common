"""Read only Codex's official account/rateLimits/read over a private stdio server."""
import json
import queue
import shutil
import subprocess
import threading
import time

def read_quota(executable=None):
    executable=executable or shutil.which('codex')
    if not executable: raise RuntimeError('CLI Codex non disponibile')
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0)
    process=subprocess.Popen([executable,'app-server'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL,text=True,encoding='utf-8',creationflags=flags)
    messages=queue.Queue(maxsize=100)
    def receive():
        try:
            for line in process.stdout:
                try: messages.put_nowait(json.loads(line))
                except (ValueError,queue.Full): pass
        except (OSError,ValueError): pass
    reader=threading.Thread(target=receive,daemon=True);reader.start()
    def rpc(identifier,method,params):
        process.stdin.write(json.dumps(dict(id=identifier,method=method,params=params))+'\n');process.stdin.flush()
        deadline=time.monotonic()+12
        while time.monotonic()<deadline:
            if process.poll() is not None: raise RuntimeError('App Server terminato')
            try: message=messages.get(timeout=.2)
            except queue.Empty: continue
            if message.get('id')==identifier:
                if 'error' in message: raise RuntimeError('Quote non disponibili per il login corrente')
                return message.get('result') or {}
        raise RuntimeError('Timeout lettura quote Codex')
    try:
        rpc(1,'initialize',{'clientInfo':{'name':'agent_deck','version':'0.1.0'}})
        process.stdin.write('{"method":"initialized"}\n');process.stdin.flush()
        result=rpc(2,'account/rateLimits/read',{})
        # Account identifiers, login information and credits never leave this function.
        limit=result.get('rateLimits') or {}
        windows=[]
        for key in ('primary','secondary'):
            item=limit.get(key)
            if not isinstance(item,dict):continue
            used=item.get('usedPercent')
            if isinstance(used,(int,float)) and 0<=used<=100:
                windows.append(dict(name=key,remaining_percent=round(100-used,1),resets_at=item.get('resetsAt'),window_minutes=item.get('windowDurationMins')))
        return dict(provider='codex',source='codex_app_server',observed_at=time.time(),windows=windows)
    finally:
        process.stdin.close()
        try:process.wait(timeout=3)
        except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=3)
        reader.join(timeout=1);process.stdout.close()
