"""Host test and preview of the actual vendored renderers (no device access)."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT=Path(__file__).resolve().parents[2]

class NativePanelTests(unittest.TestCase):
    def test_render_touch_and_reopen_all_three_layout_families(self):
        with tempfile.TemporaryDirectory(prefix='deck-panel-') as temp:
            work=Path(temp)
            (work/'test.cpp').write_text(r'''
#include <initializer_list>
#include <fstream>
#include <vector>
#include <cassert>
#include <chrono>
#include <cstdio>
#include "layout_square.h"
#include "layout_big.h"
#include "layout_spring.h"
#include "round_arc_reference.h"
void image(const char* name,const Canvas& c){std::ofstream f(name,std::ios::binary);f<<"P6\n"<<c.w<<" "<<c.h<<"\n255\n";for(int i=0;i<c.w*c.h;i++){auto p=c.px[i];unsigned char b[3]={(unsigned char)(((p>>11)&31)*255/31),(unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)};f.write((char*)b,3);}}
int main(){
 std::vector<uint16_t> actual(480*480),expected(480*480);
 for(int i=0;i<48;i++){
   Canvas a{actual.data(),480,480},e{expected.data(),480,480};a.fill(0x1234);e.fill(0x1234);
   a.top=e.top=i%3?0:170;
   float start=-370.f+i*19.37f,sweep=i%6==0?360.f:i%5==0?250.f:1.577f+i%9;
   float r0=i%4?216.f:0.f,r1=i%4?220.f:80.f,cx=i%7?240.f:12.f,cy=i%8?240.f:-5.f;
   referenceArc(e,cx,cy,r0,r1,start,sweep,0xFFFF);fillArc(a,cx,cy,r0,r1,start,sweep,0xFFFF);
   assert(actual==expected);
 }
 Canvas bench{actual.data(),480,480};auto begin=std::chrono::steady_clock::now();
 for(int i=0;i<80;i++)referenceArc(bench,240,240,216,220,i*4.5f,1.577f,0xFFFF);
 auto middle=std::chrono::steady_clock::now();
 for(int i=0;i<80;i++)fillArc(bench,240,240,216,220,i*4.5f,1.577f,0xFFFF);
 auto end=std::chrono::steady_clock::now();
 printf("ARC_REFERENCE_US %lld ARC_BOUNDED_US %lld\n",(long long)std::chrono::duration_cast<std::chrono::microseconds>(middle-begin).count(),(long long)std::chrono::duration_cast<std::chrono::microseconds>(end-middle).count());
 // First-open state: no snapshot/agents yet, the path in the hardware core dump.
 {RoundUi emptyUi{};Fleet empty{};SpringPanel emptyPanel(layoutSpring21());RoundInput emptyIn{&empty,LinkState::Connecting,"Aetherion",DeckScope::View,1234,1000,&emptyUi,0};roundTick(emptyUi,emptyPanel,empty,1234);emptyPanel.draw(bench,emptyIn);}
 Fleet f{};f.count=f.total=6;
 const char* names[]={"Codex","Talo build","Scribe","Verifica","AMOLED UI","Dashboard"};
 for(int i=0;i<6;i++){auto &a=f.agents[i];snprintf(a.id,sizeof(a.id),"agent-%d",i);a.slot=i;copyUtf8(a.title,sizeof(a.title),names[i]);copyUtf8(a.actDetail,sizeof(a.actDetail),"Agent Deck integration");copyUtf8(a.actTool,sizeof(a.actTool),"edit");a.species=i?Species::Claude:Species::Codex;a.tool=i?ToolKind::Other:ToolKind::Codex;a.status=i==2?Status::Waiting:Status::Running;a.ctxWindow=10000;a.ctxUsed=4100;a.costUsd=-1;a.usageKnown=false;a.turnsKnown=false;a.contextCountsKnown=false;}
 std::vector<uint16_t> px(480*480);Canvas c{px.data(),368,448};PanelUi ui{};
 SquarePanel square(specAmoled18());PanelInput in{&f,LinkState::Live,"Aetherion",DeckScope::View,1234,1000,&ui,0};
 square.draw(c,in);assert(px[0]==0);image("amoled-home.ppm",c);
 square.touch(ui,f,true,90,130,DeckScope::View,1300);square.touch(ui,f,false,90,130,DeckScope::View,1350);assert(ui.screen==Screen::Card);square.draw(c,in);image("amoled-card.ppm",c);
 square.button(ui,f);assert(ui.screen==Screen::Home);
 square.touch(ui,f,true,90,350,DeckScope::View,1400);square.touch(ui,f,true,90,100,DeckScope::View,1550);square.touch(ui,f,false,90,100,DeckScope::View,1600);assert(ui.scroll>0||ui.animating);
 c.w=c.h=480;ui=PanelUi{};BigPanel big(specBig480());big.draw(c,in);image("square-home.ppm",c);
 for(int pass=0;pass<2;pass++){SpringPanel spring(layoutSpring21());RoundUi rui{};RoundInput ri{&f,LinkState::Live,"Aetherion",DeckScope::View,1234,1000,&rui,0};roundTick(rui,spring,f,1234);spring.draw(c,ri);if(!pass)image("round-home.ppm",c);spring_draw::releaseStamps();}
}
''',encoding='utf-8')
            vswhere=Path(os.environ['ProgramFiles(x86)'])/'Microsoft Visual Studio/Installer/vswhere.exe'
            vs=Path(subprocess.check_output([str(vswhere),'-latest','-products','*','-property','installationPath'],text=True).strip())
            raw=subprocess.check_output(f'cmd.exe /d /s /c ""{vs/"Common7/Tools/VsDevCmd.bat"}" -arch=x64 >nul && set"',text=True)
            env=dict(os.environ)
            for line in raw.splitlines():
                if '=' in line and not line.startswith('='):
                    k,v=line.split('=',1);env[k.upper()]=v
            compiler=sorted((vs/'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/cl.exe'))[-1]
            result=subprocess.run([str(compiler),'/nologo','/std:c++17','/EHsc','/W3','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'components/apps/agentdeck/upstream'),'/I'+str(ROOT/'tools/agent_deck/fixtures'),str(work/'test.cpp'),'/Fe:'+str(work/'test.exe')],cwd=work,env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(work/'test.exe')],cwd=work,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout)
            output=Path.home()/'.agent-deck/panel-preview';output.mkdir(parents=True,exist_ok=True)
            def chunk(kind,data):return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data)&0xffffffff)
            for ppm in work.glob('*.ppm'):
                _,dimensions,_,rgb=ppm.read_bytes().split(b'\n',3);w,h=map(int,dimensions.split())
                data=b''.join(b'\0'+rgb[i*w*3:(i+1)*w*3] for i in range(h))
                png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!2I5B',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(data))+chunk(b'IEND',b'')
                (output/(ppm.stem+'.png')).write_bytes(png)
            print('PANEL_PREVIEWS '+str(output))

if __name__=='__main__':unittest.main()
