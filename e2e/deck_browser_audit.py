#!/usr/bin/env python3
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


VIEWPORTS = [
    ("mobile-portrait", 390, 844, False),
    ("mobile-landscape", 844, 390, True),
    ("tablet", 768, 1024, False),
    ("desktop", 1440, 900, False),
]


AUDIT_SCRIPT = r'''
<script>
window.addEventListener("load",function(){
function wait(ms){return new Promise(function(resolve){setTimeout(resolve,ms);});}
function later(){return wait(760);}
function key(k){document.dispatchEvent(new KeyboardEvent("keydown",{key:k,bubbles:true,cancelable:true}));}
function touch(el,x1,y1,x2,y2){function pt(x,y){return {identifier:1,target:el,clientX:x,clientY:y,pageX:x,pageY:y,screenX:x,screenY:y};}function ev(name,touches,changed){var e;if(typeof TouchEvent==="function"){try{return new TouchEvent(name,{touches:touches,changedTouches:changed,bubbles:true,cancelable:true});}catch(_){}}e=new Event(name,{bubbles:true,cancelable:true});Object.defineProperty(e,"touches",{value:touches});Object.defineProperty(e,"changedTouches",{value:changed});return e;}var a=pt(x1,y1),b=pt(x2,y2);el.dispatchEvent(ev("touchstart",[a],[a]));el.dispatchEvent(ev("touchend",[],[b]));}
(async function(){
await later();
var deck=document.querySelector(".mdf-deck");
var slides=[].slice.call(document.querySelectorAll(".mdf-slide"));
var fs=document.querySelector(".mdf-fullscreen-button");
var failures=[];
function fail(msg){failures.push(msg);}
function clip(a,b){var left=Math.max(a.left,b.left),right=Math.min(a.right,b.right),top=Math.max(a.top,b.top),bottom=Math.min(a.bottom,b.bottom);return right>left&&bottom>top?{left:left,right:right,top:top,bottom:bottom,width:right-left,height:bottom-top}:null;}
function overlap(a,b){return !(a.right<=b.left||a.left>=b.right||a.bottom<=b.top||a.top>=b.bottom);}
function unionRects(rects){var u=null;rects.forEach(function(r){if(!u)u={left:r.left,top:r.top,right:r.right,bottom:r.bottom};else{u.left=Math.min(u.left,r.left);u.top=Math.min(u.top,r.top);u.right=Math.max(u.right,r.right);u.bottom=Math.max(u.bottom,r.bottom);}});if(u){u.width=u.right-u.left;u.height=u.bottom-u.top;}return u;}
function textElement(root,needle){return root&&[].slice.call(root.querySelectorAll("span,p,li,blockquote")).filter(function(el){return el.textContent.indexOf(needle)>=0;})[0];}
function visibleTextRects(slide,boxRect){var out=[];var root=slide.querySelector(".mdf-slide-content");if(!root)return out;var walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);var n;while((n=walker.nextNode())){if(!n.nodeValue.trim())continue;var r=document.createRange();r.selectNodeContents(n);Array.prototype.forEach.call(r.getClientRects(),function(x){var c=clip(x,boxRect);if(c&&c.width>0&&c.height>0)out.push(c);});r.detach();}return out;}
function applyFit(s){var box=s.querySelector(".mdf-slide-content");if(!box)return;var vw=Math.max(320,innerWidth||document.documentElement.clientWidth||1024);var vh=Math.max(320,innerHeight||document.documentElement.clientHeight||768);var dense=box.textContent.length>520;var scale=Math.min(vw,vh);var base=scale*(s.classList.contains("mdf-slide-front") ? .072 : (dense ? .038 : .047));if(vw<640)base=scale*(s.classList.contains("mdf-slide-front") ? .087 : (dense ? .046 : .056));if(vh<520)base=scale*(s.classList.contains("mdf-slide-front") ? .077 : (dense ? .041 : .051));var hasChart=!!box.querySelector(".mdf-chart-block");var min=vw<640?(hasChart?8:12):14;if(vh<520)min=hasChart?8:12;var body=box.querySelector(".mdf-slide-body");var inner=box.querySelector(".mdf-slide-body-inner");var size=Math.max(min,Math.round(base));box.style.setProperty("--mdf-slide-font-size",size+"px");for(;size>min;size-=1){box.style.setProperty("--mdf-slide-font-size",size+"px");if(box.scrollHeight<=box.clientHeight+4&&box.scrollWidth<=box.clientWidth+4&&(!inner||(inner.scrollHeight<=body.clientHeight+4&&inner.scrollWidth<=body.clientWidth+4)))break;}box.classList.toggle("mdf-slide-scroll-fallback",box.scrollHeight>box.clientHeight+4||box.scrollWidth>box.clientWidth+4||(inner&&(inner.scrollHeight>body.clientHeight+4||inner.scrollWidth>body.clientWidth+4)));}
if(!deck)fail("missing deck");
if(slides.length!==30)fail("expected 30 slides, got "+slides.length);
if(slides[0]&&slides[0].querySelector(".mdf-slide-number"))fail("first slide has number");
if(slides[1]&&!slides[1].querySelector(".mdf-slide-number"))fail("second slide missing number");
if(slides[1]&&slides[1].querySelector(".mdf-slide-number")&&slides[1].querySelector(".mdf-slide-number").textContent!=="02/30")fail("second slide number was not formatted as 02/30");
if(slides[29]&&slides[29].querySelector(".mdf-slide-number")&&slides[29].querySelector(".mdf-slide-number").textContent!=="30/30")fail("last slide number was not formatted as 30/30");
if(deck.dataset.current!=="3")fail("initial hash did not restore slide 3");
if(slides[2]&&slides[2].getAttribute("aria-hidden")!=="false")fail("initial hash did not expose slide 3");
if(slides[2]&&"inert" in slides[2]&&slides[2].inert)fail("active slide is inert after hash restore");
var inertInactive=slides.filter(function(s,i){return i!==2&&("inert" in s)&&!s.inert;});
if(inertInactive.length)fail("inactive slides are not inert after hash restore: "+inertInactive.length);
var inactiveTabbables=[].slice.call(document.querySelectorAll('.mdf-slide[aria-hidden="true"] a[href],.mdf-slide[aria-hidden="true"] button,.mdf-slide[aria-hidden="true"] input,.mdf-slide[aria-hidden="true"] select,.mdf-slide[aria-hidden="true"] textarea,.mdf-slide[aria-hidden="true"] details>summary')).filter(function(el){return el.tabIndex>=0;});
if(inactiveTabbables.length)fail("inactive slides expose tabbable controls: "+inactiveTabbables.length);
if(fs){
var fsStyle=getComputedStyle(fs),fsRect=fs.getBoundingClientRect();
if(fsStyle.opacity!=="0")fail("fullscreen corner control is visible via opacity "+fsStyle.opacity);
if(fsStyle.fontSize!=="0px")fail("fullscreen corner control exposes text-sized UI: "+fsStyle.fontSize);
if(fsStyle.backgroundColor!=="rgba(0, 0, 0, 0)"&&fsStyle.backgroundColor!=="transparent")fail("fullscreen corner control has visible background "+fsStyle.backgroundColor);
if(fsRect.left>1||innerHeight-fsRect.bottom>1)fail("fullscreen corner control is not anchored bottom-left");
if(fsRect.width<48||fsRect.height<48)fail("fullscreen corner control hit target is too small: "+fsRect.width+"x"+fsRect.height);
if(fsRect.width>96||fsRect.height>96)fail("fullscreen corner control hit target is too large: "+fsRect.width+"x"+fsRect.height);
deck.requestFullscreen=undefined;deck.webkitRequestFullscreen=undefined;
key("f");
if(!deck.classList.contains("mdf-fallback-fullscreen"))fail("f key did not enter fallback fullscreen");
fs.click();
if(deck.classList.contains("mdf-fallback-fullscreen"))fail("fullscreen corner control did not exit fallback fullscreen");
}else fail("missing fullscreen corner control");
await wait(2800);
if(!deck.classList.contains("mdf-cursor-hidden"))fail("cursor did not hide after inactivity");
if(getComputedStyle(deck).cursor!=="none")fail("hidden cursor style is not none: "+getComputedStyle(deck).cursor);
window.dispatchEvent(new MouseEvent("mousemove",{clientX:Math.floor(innerWidth/2),clientY:Math.floor(innerHeight/2),bubbles:true}));
await wait(50);
if(deck.classList.contains("mdf-cursor-hidden"))fail("cursor did not show after mouse movement");
await wait(2600);
if(!deck.classList.contains("mdf-cursor-hidden"))fail("cursor did not hide again after mouse movement");
key("End");
await later();
if(deck.dataset.current!==String(slides.length))fail("End key did not select final slide");
if(location.hash!=="#slide-"+slides.length)fail("End key did not update hash");
key("g");
await later();
if(deck.dataset.current!=="1")fail("g key did not select first slide");
key("PageDown");
await later();
if(deck.dataset.current!=="2")fail("PageDown did not advance");
key("PageUp");
await later();
if(deck.dataset.current!=="1")fail("PageUp did not go back");
key("ArrowDown");
await later();
if(deck.dataset.current!=="2")fail("ArrowDown did not advance");
key("ArrowUp");
await later();
if(deck.dataset.current!=="1")fail("ArrowUp did not go back");
key("j");
await later();
if(deck.dataset.current!=="2")fail("j key did not advance");
key("k");
await later();
if(deck.dataset.current!=="1")fail("k key did not go back");
key("G");
await later();
if(deck.dataset.current!==String(slides.length))fail("G key did not select final slide");
key("Home");
await later();
if(deck.dataset.current!=="1")fail("Home key did not select first slide");
key("ArrowRight");
if(deck.dataset.transition==="cross"){
setTimeout(function(){
var out=slides[0],inc=slides[1];
var os=out&&getComputedStyle(out),is=inc&&getComputedStyle(inc);
if(!os||!is)fail("crossfade slides missing during transition");
else{
if(os.visibility!=="visible"||is.visibility!=="visible")fail("crossfade does not keep both slides visible");
if(os.transitionDuration.indexOf("1.6s")<0||is.transitionDuration.indexOf("1.6s")<0)fail("crossfade duration is not 1.6s");
}
},360);
setTimeout(function(){
var out=slides[0],inc=slides[1];
var os=out&&getComputedStyle(out),is=inc&&getComputedStyle(inc);
if(!os||!is)fail("crossfade slides missing late in transition");
else if(os.visibility!=="visible"||is.visibility!=="visible")fail("crossfade did not remain visible through the long transition");
},2400);
}
if(deck.dataset.transition==="fade"&&window.matchMedia&&matchMedia("(prefers-reduced-motion: reduce)").matches){
setTimeout(function(){
if(deck.dataset.current!=="2")fail("reduced-motion fade did not advance promptly");
if(deck.classList.contains("mdf-blackout"))fail("reduced-motion fade entered blackout transition");
},80);
}
if(deck.dataset.transition==="fade"){
if(!(window.matchMedia&&matchMedia("(prefers-reduced-motion: reduce)").matches)){
setTimeout(function(){
if(!deck.classList.contains("mdf-blackout"))fail("fade transition did not enter blackout during fade-out");
if(deck.dataset.current!=="1")fail("fade transition advanced before fade-out completed");
},260);
setTimeout(function(){
if(slides[1]&&getComputedStyle(slides[1]).visibility!=="visible")fail("fade-in slide is hidden during fade-in");
if(deck.classList.contains("mdf-blackout"))fail("fade transition stayed blacked out after fade-in started");
if(deck.dataset.current!=="2")fail("fade transition did not advance after blackout delay");
},620);
}
}
if(deck.dataset.transition==="hard"){
setTimeout(function(){
var s=slides[1]&&getComputedStyle(slides[1]);
if(deck.dataset.current!=="2")fail("hard transition did not advance immediately");
if(s&&s.transitionDuration!=="0s")fail("hard transition duration is not zero: "+s.transitionDuration);
},40);
}
await later();
if(deck.dataset.current!=="2")fail("ArrowRight did not advance");
key("o");
await later();
if(deck.dataset.current!=="1")fail("o key did not return to previous position");
touch(deck,120,120,20,120);
await later();
if(deck.dataset.current!=="2")fail("swipe left did not advance");
touch(deck,20,120,120,120);
await later();
if(deck.dataset.current!=="1")fail("swipe right did not go back");
touch(deck,120,180,120,60);
await later();
if(deck.dataset.current!=="2")fail("swipe up did not advance");
touch(deck,120,60,120,180);
await later();
if(deck.dataset.current!=="1")fail("swipe down did not go back");
touch(deck,100,100,150,140);
await later();
if(deck.dataset.current!=="1")fail("diagonal swipe changed slide");
location.hash="slide-0";
dispatchEvent(new HashChangeEvent("hashchange"));
await later();
if(deck.dataset.current!=="1")fail("low out-of-range hash did not clamp to first slide");
location.hash="slide-999";
dispatchEvent(new HashChangeEvent("hashchange"));
await later();
if(deck.dataset.current!==String(slides.length))fail("high out-of-range hash did not clamp to final slide");
location.hash="slide-6";
dispatchEvent(new HashChangeEvent("hashchange"));
await later();
if(deck.dataset.current!=="6")fail("hash navigation to dense slide failed");
var scrollBox=slides[5]&&slides[5].querySelector(".mdf-slide-content");
if(!scrollBox)fail("dense slide missing scroll box");
if(scrollBox){
scrollBox.style.height="120px";scrollBox.style.maxHeight="120px";scrollBox.style.overflow="auto";
if(scrollBox.scrollHeight<=scrollBox.clientHeight+4)fail("dense slide could not be forced scrollable");
scrollBox.scrollTop=Math.max(8,Math.min(80,scrollBox.scrollHeight-scrollBox.clientHeight-16));
touch(deck,120,180,120,60);
}
await later();
if(deck.dataset.current!=="6")fail("vertical swipe in middle of scrollable slide advanced deck");
if(scrollBox)scrollBox.scrollTop=scrollBox.scrollHeight;
touch(deck,120,180,120,60);
await later();
if(deck.dataset.current!=="7")fail("vertical swipe at scrollable slide bottom did not advance");
if(scrollBox)scrollBox.removeAttribute("style");
location.hash="slide-24";
dispatchEvent(new HashChangeEvent("hashchange"));
await later();
if(deck.dataset.current!=="24")fail("hash navigation to link slide failed");
var link=slides[23]&&slides[23].querySelector("a[href]");
if(!link)fail("link slide missing active link");
if(link&&link.getAttribute("target")!=="_blank")fail("deck link does not open in a new tab");
if(link&&link.getAttribute("rel")!=="noopener noreferrer")fail("deck link missing opener protection");
if(link&&link.tabIndex<0)fail("active link slide did not restore link tab order");
if(link){link.focus();link.dispatchEvent(new KeyboardEvent("keydown",{key:" ",bubbles:true,cancelable:true}));}
if(deck.dataset.current!=="24")fail("space on focused link advanced deck");
if(link)link.blur();
location.hash="slide-25";
dispatchEvent(new HashChangeEvent("hashchange"));
await later();
if(deck.dataset.current!=="25")fail("hash navigation away from link slide failed");
if(link&&link.tabIndex>=0)fail("inactive link slide remained tabbable after navigation away");
if(slides[23]&&"inert" in slides[23]&&!slides[23].inert)fail("inactive link slide was not inert after navigation away");
location.hash="slide-24";
dispatchEvent(new HashChangeEvent("hashchange"));
await later();
if(deck.dataset.current!=="24")fail("hash navigation back to link slide failed");
if(link&&link.tabIndex<0)fail("link slide tab order was not restored after returning");
if(slides[23]&&"inert" in slides[23]&&slides[23].inert)fail("active link slide stayed inert after returning");
var runtimeRows=[];
slides.forEach(function(s,i){
location.hash="slide-"+String(i+1);
dispatchEvent(new HashChangeEvent("hashchange"));
if(deck.dataset.current!==String(i+1))fail("runtime hash navigation failed for slide "+(i+1));
var box=s.querySelector(".mdf-slide-content");
var nr=s.querySelector(".mdf-slide-number");
var br=box?box.getBoundingClientRect():{left:0,top:0,right:0,bottom:0};
var text=visibleTextRects(s,br);
var fr=fs?fs.getBoundingClientRect():null;
var nrRect=nr?nr.getBoundingClientRect():null;
var fitSize=box?box.style.getPropertyValue("--mdf-slide-font-size"):"";
if(box&&!fitSize)fail("runtime fit did not set slide font size for slide "+(i+1));
runtimeRows.push({slide:i+1,font:box?getComputedStyle(box).fontSize:"",fit:fitSize,sw:box?box.scrollWidth:0,cw:box?box.clientWidth:0,sh:box?box.scrollHeight:0,ch:box?box.clientHeight:0,overflowX:box?box.scrollWidth>box.clientWidth+4:false,overflowY:box?box.scrollHeight>box.clientHeight+4:false,fsHits:fr?text.filter(function(r){return overlap(r,fr);}).length:0,nrHits:nrRect?text.filter(function(r){return overlap(r,nrRect);}).length:0});
});
var rows=[];
slides.forEach(function(s,i){
slides.forEach(function(x,j){x.setAttribute("aria-hidden",i===j?"false":"true");});
applyFit(s);
var box=s.querySelector(".mdf-slide-content");
var nr=s.querySelector(".mdf-slide-number");
var br=box?box.getBoundingClientRect():{left:0,top:0,right:0,bottom:0};
var text=visibleTextRects(s,br);
var fr=fs?fs.getBoundingClientRect():null;
var nrRect=nr?nr.getBoundingClientRect():null;
var body=box?box.querySelector(".mdf-slide-body"):null;
var inner=box?box.querySelector(".mdf-slide-body-inner"):null;
var header=s.querySelector(".mdf-slide-header .mdf-heading");
if(i>0&&header){
var slideRectForHeader=s.getBoundingClientRect(),headerRect=header.getBoundingClientRect();
var expectedHeaderLeft=slideRectForHeader.left+parseFloat(getComputedStyle(s).paddingLeft||"0");
if(Math.abs(headerRect.left-expectedHeaderLeft)>2)fail("slide "+(i+1)+" header moved from left slide padding: "+(headerRect.left-expectedHeaderLeft));
}
if(i===0&&box){
var slideRect=s.getBoundingClientRect(),frontText=unionRects(text);
if(!frontText)fail("front slide has no visible text rects");
else{
var frontX=Math.abs((frontText.left+frontText.width/2)-(slideRect.left+slideRect.width/2));
var frontY=Math.abs((frontText.top+frontText.height/2)-(slideRect.top+slideRect.height/2));
if(frontX>Math.max(8,slideRect.width*.06))fail("front slide visible content is not horizontally centered: "+frontX);
if(frontY>Math.max(8,slideRect.height*.06))fail("front slide visible content is not vertically centered: "+frontY);
}
}
if((i===3||i===13)&&body&&inner&&!box.classList.contains("mdf-slide-scroll-fallback")){
var bodyRect=body.getBoundingClientRect(),innerRect=inner.getBoundingClientRect();
var delta=Math.abs((innerRect.top+innerRect.height/2)-(bodyRect.top+bodyRect.height/2));
if(delta>2)fail("slide "+(i+1)+" body is not vertically centered: "+delta);
if(getComputedStyle(body).flexDirection!=="column")fail("slide "+(i+1)+" body flex direction is not vertical");
}
if(box&&box.querySelector(".mdf-chart-block")){
var chartBlock=box.querySelector(".mdf-chart-block");
var slideRectForChart=s.getBoundingClientRect(),chartRect=chartBlock.getBoundingClientRect();
var chartDelta=Math.abs((chartRect.left+chartRect.width/2)-(slideRectForChart.left+slideRectForChart.width/2));
if(chartDelta>Math.max(8,slideRectForChart.width*.035))fail("slide "+(i+1)+" chart block is not horizontally centered: "+chartDelta);
}
rows.push({slide:i+1,font:box?getComputedStyle(box).fontSize:"",sw:box?box.scrollWidth:0,cw:box?box.clientWidth:0,sh:box?box.scrollHeight:0,ch:box?box.clientHeight:0,overflowX:box?box.scrollWidth>box.clientWidth+4:false,overflowY:box?box.scrollHeight>box.clientHeight+4:false,fsHits:fr?text.filter(function(r){return overlap(r,fr);}).length:0,nrHits:nrRect?text.filter(function(r){return overlap(r,nrRect);}).length:0});
});
if(slides[2]&&slides[3]&&slides[4]){
var h3=slides[2].querySelector(".mdf-heading"),h4=slides[3].querySelector(".mdf-heading"),h5=slides[4].querySelector(".mdf-heading");
if(!h3||!h4||!h5)fail("heading stability slides are missing first headings");
else{
var hs3=parseFloat(getComputedStyle(h3).fontSize),hs4=parseFloat(getComputedStyle(h4).fontSize),hs5=parseFloat(getComputedStyle(h5).fontSize);
if(Math.abs(hs3-hs4)>.5||Math.abs(hs4-hs5)>.5)fail("first-level heading size varied across slides: "+hs3+","+hs4+","+hs5);
if(innerWidth>=1000&&hs3<Math.min(innerWidth,innerHeight)*.07)fail("desktop first-level heading is too small for viewport: "+hs3);
}
var stable=slides[2],stableBody=stable&&stable.querySelector(".mdf-slide-body");
if(stableBody){
var stableHeads=[].slice.call(stableBody.querySelectorAll(".mdf-heading"));
var stableSizes=stableHeads.map(function(h){return parseFloat(getComputedStyle(h).fontSize);});
if(stableSizes.length<5)fail("stable heading scale slide is missing body headings");
else{
for(var si=1;si<stableSizes.length;si++){
if(stableSizes[si]>stableSizes[si-1]+.5)fail("stable heading body scale increases at index "+si+": "+stableSizes.join(","));
}
if(innerWidth>=1000){
if(stableSizes[2]<stableSizes[1]*.65)fail("deck h4 collapsed relative to h3: "+stableSizes.join(","));
if(stableSizes[4]<stableSizes[1]*.45)fail("deck h6 collapsed relative to h3: "+stableSizes.join(","));
var stableParagraph=textElement(stable,"ATX headings should remain stable");
var stableParagraphSize=stableParagraph?parseFloat(getComputedStyle(stableParagraph).fontSize):0;
if(stableParagraphSize&&stableSizes[4]<stableParagraphSize*.95)fail("deck h6 is smaller than stable slide body text: "+stableSizes.join(",")+" body="+stableParagraphSize);
}
}
}
var split=slides[3];
if(split){
var splitHeader=split.querySelector(".mdf-slide-header .mdf-heading");
var splitBodyHeads=[].slice.call(split.querySelectorAll(".mdf-slide-body .mdf-heading"));
var splitParagraph=textElement(split,"This paragraph verifies");
if(!splitHeader)fail("header/body path slide is missing promoted slide header");
if(splitBodyHeads.length<5)fail("header/body path slide is missing body heading examples");
if(splitHeader&&splitBodyHeads[0]){
var splitHeaderSize=parseFloat(getComputedStyle(splitHeader).fontSize);
var splitBodyH1Size=parseFloat(getComputedStyle(splitBodyHeads[0]).fontSize);
if(splitBodyH1Size<splitHeaderSize*.6)fail("body h1 collapsed relative to slide header h1: "+splitBodyH1Size+" < "+splitHeaderSize);
}
if(splitBodyHeads.length>=5&&splitParagraph){
var splitParagraphSize=parseFloat(getComputedStyle(splitParagraph).fontSize);
for(var bi=2;bi<5;bi++){
var bodyHeadingSize=parseFloat(getComputedStyle(splitBodyHeads[bi]).fontSize);
if(bodyHeadingSize+0.5<splitParagraphSize)fail("body heading "+(bi+1)+" is smaller than paragraph text: "+bodyHeadingSize+" < "+splitParagraphSize);
}
var splitBox=split.querySelector(".mdf-slide-content");
if(splitBox&&innerWidth>=1000&&splitBox.scrollHeight<splitBox.clientHeight*.88)fail("header/body path slide body underused available height: "+splitBox.scrollHeight+" < "+splitBox.clientHeight);
}
}
var sparse=textElement(slides[4],"One short paragraph");
var dense=textElement(slides[5],"intentionally contains enough body content");
if(!sparse||!dense)fail("body autofit comparison text is missing");
else{
var sparseSize=parseFloat(getComputedStyle(sparse).fontSize),denseSize=parseFloat(getComputedStyle(dense).fontSize);
if(!(sparseSize>denseSize+1))fail("sparse body text did not autofit larger than dense body text: "+sparseSize+" <= "+denseSize);
if(innerWidth>=1000&&sparseSize<Math.min(innerWidth,innerHeight)*.04)fail("desktop sparse body text is too small for viewport: "+sparseSize);
if(innerWidth>=1000&&denseSize<Math.min(innerWidth,innerHeight)*.031)fail("desktop dense body text is too small for viewport: "+denseSize);
}
}
if(slides[27]&&slides[28]){
var h2slide=slides[27],h3slide=slides[28];
var h2Header=h2slide.querySelector(".mdf-slide-header .mdf-heading");
var h3Header=h3slide.querySelector(".mdf-slide-header .mdf-heading");
if(!h2Header||h2Header.textContent.indexOf("## Second-Level Slide Header")<0)fail("first ## heading was not promoted to slide header");
if(!h3Header||h3Header.textContent.indexOf("### Third-Level Slide Header")<0)fail("first ### heading was not promoted to slide header");
if(h2slide.querySelector(".mdf-slide-body .mdf-heading")||h3slide.querySelector(".mdf-slide-body .mdf-heading"))fail("first lower-level heading leaked into slide body");
}
var result={failures:failures,rows:rows,runtimeRows:runtimeRows};
document.head.innerHTML="";
document.body.innerHTML="<pre id=\"audit-results\">"+JSON.stringify(result).replace(/&/g,"&amp;").replace(/</g,"&lt;")+"</pre>";
})();
});
</script>
'''


CENTER_FRONT_TEXT_AUDIT_SCRIPT = r'''
<script>
window.addEventListener("load",function(){
function wait(ms){return new Promise(function(resolve){setTimeout(resolve,ms);});}
(async function(){
await wait(760);
var failures=[];
function fail(msg){failures.push(msg);}
var deck=document.querySelector(".mdf-deck");
var front=document.querySelector('.mdf-slide[data-slide="1"]');
var second=document.querySelector('.mdf-slide[data-slide="2"]');
function textNode(root, needle){return root&&[].slice.call(root.querySelectorAll("span,p,li,blockquote")).filter(function(el){return el.textContent.indexOf(needle)>=0;})[0];}
var frontParagraph=textNode(front,"front paragraph");
var secondParagraph=textNode(second,"later paragraph");
var shouldCenter=deck&&deck.getAttribute("data-audit-center-front-text")==="1";
if(!deck)fail("missing deck");
if(!frontParagraph)fail("front paragraph missing");
if(!secondParagraph)fail("second paragraph missing");
if(shouldCenter&&front&&!front.classList.contains("mdf-center-front-text"))fail("front slide missing center text class");
if(!shouldCenter&&front&&front.classList.contains("mdf-center-front-text"))fail("front slide unexpectedly has center text class");
if(frontParagraph){
var frontAlign=getComputedStyle(frontParagraph).textAlign;
if(shouldCenter&&frontAlign!=="center")fail("center-front paragraph alignment is "+frontAlign);
if(!shouldCenter&&frontAlign==="center")fail("default front paragraph was center-aligned");
}
if(secondParagraph&&getComputedStyle(secondParagraph).textAlign==="center")fail("second slide paragraph was center-aligned");
document.head.innerHTML="";
document.body.innerHTML="<pre id=\"audit-results\">"+JSON.stringify({failures:failures}).replace(/&/g,"&amp;").replace(/</g,"&lt;")+"</pre>";
})();
});
</script>
'''


def find_chrome():
    env = os.environ.get("CHROME")
    if env:
        return env
    for name in ("google-chrome", "chromium", "chromium-browser"):
        path = shutil.which(name)
        if path:
            return path
    return None


def run(cmd):
    subprocess.run(cmd, check=True, text=True)


def inject_audit(src, dst, script=AUDIT_SCRIPT):
    text = Path(src).read_text()
    if "</body>" not in text:
        raise RuntimeError("deck html has no </body>")
    Path(dst).write_text(text.replace("</body>", script + "</body>"))


def audit_viewport(chrome, audit_html, transition, name, width, height, allow_scroll, reduced_motion=False):
    cmd = [
            chrome,
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--virtual-time-budget=36000",
            "--window-size=%d,%d" % (width, height),
            "--dump-dom",
            Path(audit_html).resolve().as_uri() + "#slide-3",
    ]
    if reduced_motion:
        cmd.insert(4, "--force-prefers-reduced-motion=reduce")
    proc = subprocess.run(
        cmd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    match = re.search(r'<pre id="audit-results">(.*?)</pre>', proc.stdout, re.S)
    if not match:
        raise RuntimeError("%s/%s: audit results missing" % (transition, name))
    result = json.loads(html.unescape(match.group(1)))
    failures = list(result["failures"])
    hard = []
    scroll = []
    for row in result["rows"]:
        if row["overflowX"] or row["fsHits"] or row["nrHits"]:
            hard.append(row)
        if row["overflowY"]:
            scroll.append((row["slide"], row["font"], row["sh"], row["ch"]))
    runtime_scroll = []
    for row in result.get("runtimeRows", []):
        if row["overflowX"] or row["fsHits"] or row["nrHits"]:
            hard.append(row)
        if row["overflowY"]:
            runtime_scroll.append((row["slide"], row["font"], row["fit"], row["sh"], row["ch"]))
    if hard:
        failures.append("geometry hard failures: %s" % hard)
    if scroll and not allow_scroll:
        failures.append("unexpected vertical scroll: %s" % scroll)
    if runtime_scroll and not allow_scroll:
        failures.append("unexpected runtime vertical scroll: %s" % runtime_scroll)
    if failures:
        raise RuntimeError("%s/%s %dx%d failed: %s" % (transition, name, width, height, "; ".join(failures)))
    label = "%s %s" % (transition, name)
    if reduced_motion:
        label += " reduced-motion"
    print("%s %dx%d: %d slides, %d scroll fallbacks" % (label, width, height, len(result["rows"]), len(scroll)))


def audit_no_javascript(deck_html):
    text = Path(deck_html).read_text()
    failures = []
    if '<section class="mdf-slide mdf-slide-front" data-slide="1" aria-hidden="false">' not in text:
        failures.append("first slide is not statically exposed")
    if '<section class="mdf-slide" data-slide="2" aria-hidden="true">' not in text:
        failures.append("second slide is not statically hidden")
    if "# libmdf Deck Mode" not in text:
        failures.append("first slide content missing without javascript")
    if 'id="audit-results"' in text:
        failures.append("javascript audit ran despite disabled javascript")
    if failures:
        raise RuntimeError("no-js mobile-portrait failed: %s" % "; ".join(failures))
    print("no-js static fallback: first slide exposed")


def audit_center_front_text(chrome, cmdf, tmp):
    markdown = Path(tmp) / "center-front-text.md"
    markdown.write_text("# Front\n\nA front paragraph should be centered only when requested.\n\n---\n\n# Later\n\nA later paragraph stays normally aligned.\n")
    for enabled in (False, True):
        deck = Path(tmp) / ("center-front-text-%s.html" % ("on" if enabled else "off"))
        audit = Path(tmp) / ("center-front-text-%s-audit.html" % ("on" if enabled else "off"))
        cmd = [cmdf, "--deck", "-o", str(deck)]
        if enabled:
            cmd.insert(2, "--deck-center-front-text")
        cmd.append(str(markdown))
        run(cmd)
        text = deck.read_text()
        marker = '<main class="mdf-deck"'
        text = text.replace(marker, marker + ' data-audit-center-front-text="%d"' % (1 if enabled else 0), 1)
        audit.write_text(text.replace("</body>", CENTER_FRONT_TEXT_AUDIT_SCRIPT + "</body>"))
        proc = subprocess.run(
            [
                chrome,
                "--headless=new",
                "--disable-gpu",
                "--no-sandbox",
                "--virtual-time-budget=8000",
                "--window-size=1440,900",
                "--dump-dom",
                audit.resolve().as_uri() + "#slide-1",
            ],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        match = re.search(r'<pre id="audit-results">(.*?)</pre>', proc.stdout, re.S)
        if not match:
            raise RuntimeError("center-front-text audit results missing")
        result = json.loads(html.unescape(match.group(1)))
        failures = list(result["failures"])
        if failures:
            raise RuntimeError("center-front-text %s failed: %s" % ("on" if enabled else "off", "; ".join(failures)))
    print("center-front-text desktop 1440x900: flag centers only first-slide paragraphs")


def main(argv):
    if len(argv) != 3:
        print("usage: deck_browser_audit.py CMD_F DECK_MARKDOWN", file=sys.stderr)
        return 2
    chrome = find_chrome()
    if chrome is None:
        print("no Chrome/Chromium found; skipping browser deck audit")
        return 77
    cmdf = argv[1]
    markdown = argv[2]
    with tempfile.TemporaryDirectory(prefix="libmdf-deck-audit-") as tmp:
        for transition in ("fade", "cross", "hard"):
            deck = Path(tmp) / ("%s.html" % transition)
            audit = Path(tmp) / ("%s-audit.html" % transition)
            run([cmdf, "--deck", "--slide-numbers", "-x", transition, "-o", str(deck), markdown])
            if transition == "fade":
                audit_no_javascript(deck)
            inject_audit(deck, audit)
            for viewport in VIEWPORTS:
                audit_viewport(chrome, audit, transition, *viewport)
            if transition == "fade":
                audit_viewport(chrome, audit, transition, "mobile-portrait", 390, 844, False, True)
        audit_center_front_text(chrome, cmdf, tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
