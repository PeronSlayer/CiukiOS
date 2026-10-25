"""fx-14 synthetic pages shaped like ordinary web pages (sizes, node counts,
inline scripts, nesting), written for the tests: no real site's content."""
import random

WORDS=("ciuki","retro","window","driver","packet","kernel","sound","floppy","mouse","pixel",
       "screen","modem","archive","printer","joystick","network","memory","cache","garden",
       "river","market","weather","station","library","harbour","valley","signal","coffee",
       "planet","engine","bridge","forest","pocket","silver","orange","winter","summer")

def words(rng,count):
    return " ".join(rng.choice(WORDS) for _ in range(count))

def script(rng,size):
    """Minified-looking JavaScript of about size bytes (never executed)."""
    out=[];total=0;i=0
    while total<size:
        part=(f"var _{i}=function(a,b){{if(a<{rng.randrange(999)}&&b)return a*{rng.randrange(9)+1}+\"{rng.choice(WORDS)}\";"
              f"for(var c=0;c<a.length;c++)b[c]=a[c]^{rng.randrange(255)};return b}};")
        out.append(part);total+=len(part);i+=1
    return "".join(out)

def data_script(rng,size):
    """A JSON state blob of about size bytes, as server-rendered pages embed."""
    out=['{"props":{"items":[']; total=0; i=0
    while total<size:
        part=f'{{"id":{i},"title":"{words(rng,6)}","url":"/item/{i}","tags":["{rng.choice(WORDS)}","{rng.choice(WORDS)}"]}},'
        out.append(part);total+=len(part);i+=1
    out.append('{}]}}');return "".join(out)

def search_home(seed=1):
    """Search-engine home page: about 80 KB, more than 48 KB of inline script."""
    rng=random.Random(seed)
    head=("<!doctype html><html lang=en><head><meta charset=utf-8><title>Search Home</title>"
          +"".join(f'<meta name="m{i}" content="{words(rng,8)}">' for i in range(12))
          +"<style>"+"".join(f".c{i}{{margin:{i%7}px;color:#{i*37%4096:03x};font-size:{12+i%5}px}}" for i in range(220))+"</style>"
          +"<script nonce=x>"+script(rng,30000)+"</script><script>"+script(rng,22000)+"</script></head>")
    nav="".join(f'<a class="c{i}" href="/s{i}?hl=en&amp;q={rng.choice(WORDS)}">{words(rng,1)}</a>' for i in range(30))
    body=('<body><div id=top><div class=bar>'+nav+'</div></div><center><div id=logo><span>Ciuki</span></div>'
          '<form action="/search" method=get><input name=q size=40 maxlength=2048 aria-label="Search">'
          '<input type=submit value="Search"><input type=submit name=lucky value="Lucky"></form></center>'
          +"".join(f'<div class=c{i}><a href="/p{i}">{words(rng,3)}</a></div>' for i in range(40))
          +"<script>"+script(rng,8000)+"</script></body></html>")
    return head+body

def news_front(seed=2,teasers=600):
    """News front page: about 230 KB, 15,000-20,000 nodes, a 60 KB JSON blob."""
    rng=random.Random(seed)
    parts=["<!doctype html><html><head><meta charset=utf-8><title>News Front</title>",
           "<style>"+"".join(f".t{i}{{padding:{i%5}px;border-bottom:1px solid #ccc}}" for i in range(120))+"</style></head><body>",
           '<header><nav><ul>'+"".join(f'<li><a href="/{w}">{w}</a></li>' for w in WORDS[:20])+'</ul></nav></header><main>']
    for i in range(teasers):
        parts.append(f'<div class="t{i%120}" data-id="{i}"><a href="/story/{i}" class="link"><h3><span>{words(rng,7)}</span></h3></a>'
                     f'<p><span>{words(rng,14)}</span></p><ul class=meta><li><time>{i%24}:{i%60:02d}</time></li><li><a href="/topic/{i%40}">{rng.choice(WORDS)}</a></li></ul></div>')
    parts.append('</main><footer>'+"".join(f'<a href="/f{i}">{words(rng,2)}</a> ' for i in range(60))+'</footer>')
    parts.append('<script id=state type="application/json">'+data_script(rng,60000)+'</script></body></html>')
    return "".join(parts)

def article(seed=3,size=300_000):
    """Encyclopedia article: about 300 KB, long paragraphs, a contents list,
    an info table and references."""
    rng=random.Random(seed)
    parts=["<!doctype html><html><head><meta charset=utf-8><title>Long Article</title></head><body>",
           '<div id=content><h1>Ciuki</h1><table class=infobox>'+"".join(f'<tr><th>{w}</th><td>{words(rng,4)}</td></tr>' for w in WORDS[:16])+'</table>',
           '<div id=toc><ol>'+"".join(f'<li><a href="#s{i}">{words(rng,3)}</a></li>' for i in range(40))+'</ol></div>']
    total=sum(map(len,parts));section=0
    while total<size:
        if section%8==0:parts.append(f'<h2 id="s{section//8}">{words(rng,4)}</h2>')
        paragraph=f'<p>{words(rng,60)} <a href="/wiki/{rng.choice(WORDS)}">{words(rng,2)}</a> {words(rng,90)}<sup><a href="#r{section}">[{section}]</a></sup> {words(rng,40)}</p>'
        parts.append(paragraph);total+=len(paragraph);section+=1
    parts.append('<ol class=references>'+"".join(f'<li id="r{i}">{words(rng,10)}</li>' for i in range(section))+'</ol></div></body></html>')
    return "".join(parts)

def forum_thread(seed=4,posts=60):
    """Forum thread with table layout, quotes nested three deep, signatures."""
    rng=random.Random(seed)
    parts=["<!doctype html><html><head><title>Forum Thread</title></head><body><table width=100% cellspacing=1 cellpadding=4>"]
    for i in range(posts):
        quote=f'<blockquote><div>{words(rng,10)}<blockquote><div>{words(rng,10)}<blockquote>{words(rng,8)}</blockquote></div></blockquote></div></blockquote>' if i%3==0 else ""
        parts.append(f'<tr><td valign=top width=150><b>user{i%17}</b><br><small>Posts: {i*13}</small></td><td>'
                     f'<table width=100%><tr><td><small>#{i}</small></td><td align=right><a href="?p={i}">link</a></td></tr></table>'
                     f'{quote}'+"".join(f'<p>{words(rng,45)}</p>' for _ in range(4))+
                     f'<hr><small><i>{words(rng,12)}</i></small></td></tr>')
    parts.append("</table></body></html>")
    return "".join(parts)

def nested(depth=100):
    """Nesting deeper than real pages (Lighthouse flags more than 32)."""
    return "<!doctype html><title>Nested</title>"+"<div>"*depth+"deep"+"</div>"*depth

def oversized(elements=60_000):
    """Far more elements than the 128 MiB baseline budget admits."""
    return "<!doctype html><title>Oversized</title><p>"+"<b>x</b>"*elements+"</p>"

def news_front_large(seed=5):
    """The largest ordinary page: about 600 KB and 20,000 DOM nodes."""
    return news_front(seed,teasers=1300).replace("<title>News Front</title>","<title>News Front Large</title>")

def portal(seed=7,sections=30):
    """fx-nsperf: a portal page using eight text styles (three families,
    bold, italic, five sizes), the glyph cache's worst ordinary case."""
    rng=random.Random(seed)
    parts=["<!doctype html><html><head><meta charset=utf-8><title>Portal</title><style>"
           "body{font-family:sans-serif;font-size:14px} h1{font-size:30px} h2{font-size:22px;font-family:serif} h3{font-size:17px}"
           ".s{font-size:11px} .m{font-family:monospace;font-size:13px} .q{font-family:serif;font-style:italic;font-size:15px}"
           ".b{font-weight:bold;font-size:16px} .big{font-size:19px}</style></head><body><h1>"+words(rng,5)+"</h1>"]
    for _ in range(sections):
        parts.append(f"<h2>{words(rng,6)}</h2><h3>{words(rng,8)}</h3><p class=s>{words(rng,20)}</p>"
                     f"<p>{words(rng,60)} <b>{words(rng,5)}</b> <i>{words(rng,6)}</i></p><p class=q>{words(rng,30)}</p>"
                     f"<pre class=m>{words(rng,12)}\n{words(rng,12)}</pre><p class=b>{words(rng,15)}</p><p class=big>{words(rng,25)}</p>")
    parts.append("</body></html>")
    return "".join(parts)

PAGES={"search-home.html":search_home,"news-front.html":news_front,"article.html":article,
       "forum-thread.html":forum_thread,"nested-100.html":nested,"news-front-large.html":news_front_large}
TITLES=("Search Home","News Front","Long Article","Forum Thread","Nested","News Front Large")
