function trace(label, address) {
    Interceptor.attach(address, {onEnter(args) {
        send({at:label,args:[args[0].toString(),args[1].toString()],
              backtrace:Thread.backtrace(this.context,Backtracer.ACCURATE).map(DebugSymbol.fromAddress).map(String),
              stack:hexdump(this.context.esp,{length:96})});
    }});
}
for (const name of ['ExitProcess','TerminateProcess','ExitThread','FatalExit']) {
    const p=Module.findExportByName('kernel32.dll',name);
    if (p) trace(name,p);
}
for (const name of ['NtTerminateProcess','NtTerminateThread','RtlExitUserProcess']) {
    const p=Module.findExportByName('ntdll.dll',name);
    if (p) trace(name,p);
}
for (const va of [0x9e5efe,0x8f4421,0x8f4d01]) trace('site '+ptr(va),ptr(va));
trace('CoCreateInstance',Module.getExportByName('ole32.dll','CoCreateInstance'));
Interceptor.attach(Module.getExportByName('kernel32.dll','LoadLibraryA'),{onEnter(args){send({load:args[0].readCString(),from:this.returnAddress.toString()});}});
Process.setExceptionHandler(function(e){send({exception:e,backtrace:Thread.backtrace(e.context,Backtracer.ACCURATE).map(DebugSymbol.fromAddress).map(String),stack:hexdump(e.context.esp,{length:96})});return false;});
rpc.exports.state=function(){return Process.enumerateThreads();};
