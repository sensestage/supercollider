BelaScope {

	classvar <serverScopes;
	var <server, <bus, <node;

	// public interface

	*scope { |channelOffset, signals, server|
		^this.getInstance(server).scope(channelOffset, signals);
	}

	scope { |channelOffset, signals|
		
		var ugens = this.class.prInputAsAudioRateUGens(signals);

		if(ugens.notNil and: this.prIsValidScopeChannel(channelOffset, signals)){
			Out.ar(this.bus.index + channelOffset, ugens);
		};

		^signals;
	}

	maxChannels { ^this.server.options.belaMaxScopeChannels }

	// instance creation

	*initClass {
		serverScopes = IdentityDictionary[];
	}

	*new { |server|
		^this.getInstance(server);
	}

	*getInstance { |server|
		server = server ? Server.default;
		serverScopes[server] ?? {
			serverScopes[server] = super.newCopyArgs(server).init;
		}
		^serverScopes[server];
	}

	init {
		ServerBoot.add(this, this.server);
		ServerTree.add(this, this.server);
		if(this.server.serverRunning){
				this.doOnServerBoot;
				this.doOnServerTree;
		}
	}

	// bus and synth creation

	prReserveScopeBus {
		// TODO: check if bus is already reserved, or if maxChannels mismatch
		bus = Bus.audio(server, this.maxChannels);
	}

	prStartScope {
		if(node.notNil) {
			if (node.isRunning) { 
				warn("BelaScope: can't instantiate a new BelaScopeUGen, because one is already active.");
				^this
			} 
		};

		if(UGen.buildSynthDef.notNil) {
			// When BelaScope.init is called inside a SynthDef function (e.g. by UGen:belaScope),
			// this.prStartSynth breaks that SynthDef:build, because it attempts to create an inner SynthDef. 
			// Fixed by forking.
			fork{ this.prStartSynth };
		} {
			this.prStartSynth;
		}

	}

	prStartSynth {
		node = SynthDef(\bela_stethoscope) {
			BelaScopeUGen.ar(this.bus, this.maxChannels);
		}
		.play(this.server, addAction: \addAfter)
		.register;
	}
	
	doOnServerBoot { this.prReserveScopeBus; ServerBoot.remove(this) }
	doOnServerTree { this.prStartScope; ServerTree.remove(this) }

	// scope input checks

	*prInputAsAudioRateUGens { |signals|
		var arUGens = signals.asArray.collect{ |item|
			switch(item.rate)
				{ \audio }{ item } // pass
				{ \control }{ K2A.ar(item) } // convert kr to ar
				{ \scalar }{
					// convert numbers to ar UGens
					if(item.isNumber) { DC.ar(item) } { nil }
				}
				{ nil }
		};

		if(arUGens.every(_.isUGen)) {
			^arUGens;
		} {
			warn(
				"BelaScope: can't scope this signal, because not all of its elements are UGens.\nSignal: %"
				.format(signals)
			);
			^nil;
		}
	}

	prIsValidScopeChannel { |channelOffset, signals=#[]|
		if(channelOffset.isNumber.not) {			
			warn("BelaScope: channel offset must be a number, but (%) is provided.".format(channelOffset));
			^false;
		};
		if(channelOffset < 0) {			
			warn("BelaScope: channel offset must be a positive number, but (%) is provided.".format(channelOffset));
			^false;
		};
		if(channelOffset + signals.asArray.size > this.maxChannels){
			warn(
				"BelaScope: can't scope this signal to scope channel (%), max number of channels (%) exceeded.\nSignal: %"
				.format(channelOffset, this.maxChannels, signals)
			);
			^false;
		};
		^true;   
	}
}

+ UGen {
	belaScope { |scopeChannel, server|
		^BelaScope.scope(scopeChannel, this, server)
	}
}

+ Array {
	belaScope { |scopeChannel, server|
		^BelaScope.scope(scopeChannel, this, server)
	}
}
