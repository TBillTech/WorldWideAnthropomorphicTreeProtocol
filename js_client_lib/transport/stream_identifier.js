export default class StreamIdentifier {
    constructor(cid, logicalId) {
        this.cid = String(cid);
        this.logicalId = Number(logicalId);
    }

    equals(other) {
        return this.cid === other.cid && this.logicalId === other.logicalId;
    }

    compare(other) {
        if (this.cid < other.cid) return -1;
        if (this.cid > other.cid) return 1;
        return this.logicalId - other.logicalId;
    }

    toString() {
        return `${this.cid}:${this.logicalId}`;
    }
}
