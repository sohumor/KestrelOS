# Pulse streams and Talon change history

Kernel subsystems can publish typed records into independent bounded Pulse
streams. Each stream owns its sequence, retention, and wake channel; readers
can replay then follow a topic mask without consuming records for another
reader. Producers supply the storage and lock. A producer can publish while
holding the same lock that commits the state described by the record.

The system Pulse ABI is unchanged: its existing 128-record history, topic
filter, and scan cursor are one stream. A zero-capacity read follows the
current sequence without replaying the retained records.

Talon uses a separate stream for each owner. Its kernel change readers can
replay 32 metadata records even though the clipboard keeps only eight
payloads. A historical record does not make an evicted parcel retrievable.
A sequence gap means older metadata has been overwritten. Readers that
arrive before an owner publishes their first parcel can wait without taking
a clipboard owner slot. A different owner's writes do not notify them.
The public Talon payload and system Pulse interfaces are unchanged.
