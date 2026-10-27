import importlib.util
import json
import sqlite3
import tempfile
import time
import unittest
import subprocess
import sys
from test_panel import NativePanelTests
from pathlib import Path
from types import SimpleNamespace

qa_spec = importlib.util.spec_from_file_location('test_device_qa', Path(__file__).resolve().parents[1]/'dev/test_device_qa.py')
qa_tests = importlib.util.module_from_spec(qa_spec);qa_spec.loader.exec_module(qa_tests)
DeviceQATests = qa_tests.DeviceQATests

spec = importlib.util.spec_from_file_location('monitor', Path(__file__).with_name('monitor.py'))
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class MonitorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
    def tearDown(self): self.temp.cleanup()
    def transcript(self, events):
        path = self.root/'rollout.jsonl'
        from datetime import datetime, timezone
        date = datetime.now(timezone.utc).isoformat()
        path.write_text('\n'.join(json.dumps(dict(timestamp=date, **e)) for e in events), encoding='utf-8')
        return path
    def test_codex_work_wait_done_and_stale(self):
        path = self.transcript([dict(type='response_item', payload=dict(type='function_call', name='request_user_input',call_id='x'))])
        self.assertEqual(m.codex_activity(path)['status'],'needs_input')
        path = self.transcript([dict(type='event_msg',payload=dict(type='task_started'))])
        self.assertEqual(m.codex_activity(path)['status'],'working')
        self.assertEqual(m.codex_activity(path,time.time()+240)['status'],'unknown')
        path = self.transcript([dict(type='event_msg',payload=dict(type='task_complete'))])
        self.assertEqual(m.codex_activity(path)['status'],'done')
    def test_task_survives_tool_events_and_context_is_excluded(self):
        envelope='Rules\nAETHERION_CONTEXT_JSON\n'+json.dumps({'sections':{'private':'do not show'}})+'\n\nAdd mission progress'
        path=self.transcript([
            dict(type='event_msg',payload=dict(type='user_message',message=envelope)),
            dict(type='event_msg',payload=dict(type='agent_message',phase='commentary',message='Inspecting mission tracking')),
            dict(type='response_item',payload=dict(type='function_call',name='exec_command',call_id='tool'))])
        result=m.codex_activity(path)
        self.assertEqual(result['task_description'],'Add mission progress')
        self.assertEqual(result['last_update'],'Inspecting mission tracking')
        self.assertIn('exec_command',result['step']);self.assertIn('Add mission progress',result['activity'])
        self.assertNotIn('private',result['activity'])
        monitor=m.Monitor(SimpleNamespace(data=self.root,workspace=self.root,device_config=None))
        compact=json.loads(monitor.compact(dict(agents=[dict(result,id='codex:one',agent='codex',name='Codex')],jobs=[],generated_at=time.time())))
        self.assertIn('Add mission progress',compact['cards'][0]['activity'])
        self.assertEqual(m.task_description('AETHERION_CONTEXT_JSON\ninvalid'), '')

    def test_large_rollout_keeps_goal_and_latest_step(self):
        path=self.transcript([
            dict(type='response_item',payload=dict(type='message',role='user',content=[dict(type='input_text',text='Add progress indicators')])),
            dict(type='response_item',payload=dict(type='custom_tool_call_output',output='x'*300000)),
            dict(type='response_item',payload=dict(type='message',role='assistant',phase='commentary',content=[dict(type='output_text',text='Checking parser')])),
            dict(type='response_item',payload=dict(type='function_call',name='exec_command',call_id='current'))])
        item=m.codex_activity(path)
        self.assertEqual(item['task_description'],'Add progress indicators')
        self.assertEqual(item['last_update'],'Checking parser')
        self.assertIn('exec_command',item['step'])

    def test_context_is_not_cumulative_tokens(self):
        path=self.transcript([dict(type='event_msg',payload=dict(type='token_count',info=dict(model_context_window=1000,last_token_usage=dict(total_tokens=100),total_token_usage=dict(total_tokens=200000)),rate_limits=dict(primary=dict(used_percent=25,resets_at=2000000000))))])
        result=m.codex_activity(path)
        self.assertEqual(result['context_percent'],10)
        self.assertEqual(result['tokens'],200000)
        self.assertEqual(result['quota'][0]['remaining_percent'],75)
        self.assertEqual(m.quota_windows({'primary':{'used_percent':120}}),[])
    def test_assistant_question_is_visible_as_waiting_input(self):
        path=self.transcript([dict(type='response_item',payload=dict(type='function_call',name='aetherion_ask',call_id='q',arguments=json.dumps({'question':'Which device?'})))])
        result=m.codex_activity(path);self.assertEqual(result['status'],'needs_input');self.assertIn('Which device?',result['activity'])
    def test_cloud_metrics_preserve_exact_counters_without_inventing_spend(self):
        path=self.transcript([dict(type='turn_context',payload=dict(model='actual-model')),
            dict(type='event_msg',payload=dict(type='token_count',info=dict(model_context_window=1000,
                last_token_usage=dict(input_tokens=200,total_tokens=260),
                total_token_usage=dict(input_tokens=900,cached_input_tokens=600,output_tokens=70,reasoning_output_tokens=20,total_tokens=970))))])
        result=m.codex_activity(path)
        self.assertEqual(result['model'],'actual-model');self.assertEqual(result['context_percent'],20)
        self.assertEqual(result['context_used'],200);self.assertEqual(result['context_window'],1000)
        self.assertEqual(result['token_usage']['cached_input_tokens'],600)
        self.assertEqual(result['tokens'],970);self.assertIsNone(result['cost_usd'])
        self.assertEqual(result['cost_kind'],'unavailable')
    def test_readonly_database_and_path_boundary(self):
        path=self.transcript([dict(type='event_msg',payload=dict(type='task_complete'))])
        db=self.root/'state_5.sqlite'
        with sqlite3.connect(db) as c:
            c.execute('create table threads(id text, rollout_path text, updated_at integer, title text, cwd text)')
            c.execute('insert into threads values(?,?,?,?,?)',('session-1',str(path),int(time.time()),'Test','project'))
            c.execute('insert into threads values(?,?,?,?,?)',('session-2',str(self.root.parent/'outside.jsonl'),int(time.time()),'Other','project'))
        c.close()
        before=db.read_bytes()
        sessions,error=m.codex_sessions(self.root)
        self.assertIsNone(error);self.assertEqual(len(sessions),1)
        self.assertEqual(db.read_bytes(),before)
    def test_jobs_persist_without_claiming_success(self):
        store=m.Store(self.root/'data')
        store.put(dict(id='build-1',status='working',pid=None))
        self.assertEqual(store.get('build-1')['status'],'working')
        store.put(dict(id='build-1',status='error',exit_code=2))
        self.assertEqual(store.get('build-1')['exit_code'],2)
    def test_bounded_device_snapshot(self):
        args=SimpleNamespace(data=self.root/'data',workspace=self.root)
        monitor=m.Monitor(args)
        full=dict(generated_at=time.time(),agents=[dict(id=str(i),agent='codex',name='Agent',status='working',activity='x'*240,observed_at=1,repo='repo',branch='main') for i in range(60)],jobs=[])
        raw=monitor.compact(full);result=json.loads(raw)
        self.assertLessEqual(len(result['cards']),16);self.assertLessEqual(len(raw),24000)
        self.assertEqual(result['total'],1);self.assertEqual(result['total_sessions'],60)
    def test_sessions_are_grouped_as_one_agent_without_summing_usage(self):
        sessions=[dict(id='s1',agent='codex',session_id='one',status='done',observed_at=20,tokens=100,activity='old'),dict(id='s2',agent='codex',session_id='two',status='working',observed_at=10,tokens=3,activity='Current task')]
        groups=m.agent_groups(sessions)
        self.assertEqual(len(groups),1);self.assertEqual(groups[0]['session_count'],2)
        self.assertEqual(groups[0]['active_sessions'],1);self.assertEqual(groups[0]['tokens'],3)
        self.assertIn('Current task',groups[0]['activity'])
        self.assertEqual(len(sessions),2)

    def test_owner_terminal_state_overrides_unfinished_rollout_only_for_exact_session(self):
        db=sqlite3.connect(self.root/'control.sqlite')
        db.execute('CREATE TABLE entities(kind TEXT,body TEXT)')
        db.execute('INSERT INTO entities VALUES(?,?)',('chat_threads',json.dumps(dict(id='local',remote_thread_id='known',status='failed'))));db.commit();db.close()
        agents=[dict(session_id='known',status='working',task_description='Progress'),dict(session_id='other',status='working')]
        m.apply_owned_terminal_states(agents,self.root)
        self.assertEqual(agents[0]['status'],'error');self.assertEqual(agents[1]['status'],'working')
        self.assertEqual(agents[0]['owner_thread_id'],'local')

    def test_redaction(self):
        self.assertNotIn('example-private-value',m.clean('token=example-private-value'))
        self.assertEqual(m.clean('a\x00b'),'ab')
        self.assertLessEqual(len(m.clean('è'*100,100).encode('utf-8')),100)
    def test_pairing_url_boundary(self):
        self.assertEqual(m.device_url('http://192.168.10.5/'),'http://192.168.10.5')
        for url in ['http://127.0.0.1','https://192.168.10.5','http://169.254.169.254','http://example.org','http://192.168.10.5/?token=x','http://name:secret@192.168.10.5']:
            with self.assertRaises(ValueError):m.device_url(url)
    def test_missing_claude_is_not_a_fake_agent(self):
        sessions,error=m.claude_sessions(self.root)
        self.assertEqual(sessions,[]);self.assertIn('nessuna sessione',error)
    def test_claude_hooks_drive_session_state(self):
        project=self.root/'claude'/'projects'/'p';project.mkdir(parents=True)
        sid='0f0e9d1c-aaaa-bbbb-cccc-1234567890ab'
        (project/(sid+'.jsonl')).write_text(json.dumps(dict(type='assistant',cwd='C:/w/repo',message=dict(model='m',content=[])))+'\n',encoding='utf-8')
        data=self.root/'deck'
        self.assertIsNone(m.record_hook(data,dict(hook_event_name='Bogus',session_id=sid)))
        self.assertIsNone(m.record_hook(data,dict(hook_event_name='Stop',session_id='../x')))
        m.record_hook(data,dict(hook_event_name='UserPromptSubmit',session_id=sid,prompt='Fix   the build token=secret-value',cwd='C:/w/repo'))
        m.record_hook(data,dict(hook_event_name='PreToolUse',session_id=sid,tool_name='Agent',tool_input=dict(description='Find callers')))
        m.record_hook(data,dict(hook_event_name='PermissionRequest',session_id=sid,tool_name='Bash'))
        sessions,_=m.claude_sessions(self.root/'claude',hooks=data)
        self.assertEqual(sessions[0]['status'],'needs_input');self.assertEqual(sessions[0]['source'],'claude_hooks')
        self.assertEqual(sessions[0]['subagents'],['Find callers'])
        self.assertTrue(sessions[0]['goal'].startswith('Fix the build'));self.assertNotIn('secret-value',sessions[0]['goal'])
        m.record_hook(data,dict(hook_event_name='Stop',session_id=sid))
        sessions,_=m.claude_sessions(self.root/'claude',hooks=data)
        self.assertEqual(sessions[0]['status'],'done')
    def test_process_probe_never_stops_the_process(self):
        child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(10)'])
        try:
            for _ in range(3): self.assertTrue(m.process_alive(child.pid))
            time.sleep(.15)
            self.assertIsNone(child.poll())
        finally: child.terminate();child.wait()
        self.assertFalse(m.process_alive(child.pid))
    def test_real_exit_codes_are_recorded(self):
        for code in (0,7):
            args=SimpleNamespace(command=[sys.executable,'-c',f'import sys;sys.exit({code})'],data=self.root/'jobs',cwd=self.root,title='Validation',agent='codex',kind='test')
            self.assertEqual(m.tracked_run(args),code)
            job=m.Store(args.data).all()[0]
            self.assertEqual(job['exit_code'],code)
            self.assertEqual(job['status'],'done' if code==0 else 'error')

if __name__=='__main__': unittest.main()
