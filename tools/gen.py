import re,sys
src,dst=sys.argv[1],sys.argv[2]
html=open(src,encoding='utf-8').read()
html=re.sub(r'\n+','\n',html).strip()+'\n'
assert all(ord(c)<128 for c in html),'non-ascii in page'
body=len(html.encode())
hdr="HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"%body
esc=lambda s:s.replace('\\','\\\\').replace('"','\\"').replace('\r','\\r').replace('\n','\\n')
lines=[l for l in html.split('\n') if l!='']
out=['/* Generated from tools/page.html by tools/gen.py - do not edit */','#ifndef _WEB_PAGE_H_','#define _WEB_PAGE_H_','','#define WEB_PAGE \\','"'+esc(hdr)+'" \\']
for i,l in enumerate(lines):
    out.append('"'+esc(l+'\n')+'"'+(' \\' if i<len(lines)-1 else ''))
out+=['','#endif','']
open(dst,'w',encoding='utf-8').write('\n'.join(out))
print('page bytes',body)
