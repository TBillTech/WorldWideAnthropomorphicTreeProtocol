export default class Request {
    constructor({ scheme = 'https', authority = '', path = '/', method = 'GET', pri = { urgency: 0, inc: 0 } } = {}) {
        this.scheme = scheme;
        this.authority = authority;
        this.path = path;
        this.method = method;
        this.pri = { urgency: pri.urgency | 0, inc: pri.inc | 0 };
    }

    isWWATP() {
        return this.path.includes('wwatp/');
    }

    equals(other) {
        return (
            this.scheme === other.scheme &&
            this.authority === other.authority &&
            this.path === other.path &&
            this.method === other.method &&
            this.pri.urgency === other.pri.urgency &&
            this.pri.inc === other.pri.inc
        );
    }

    compare(other) {
        if (this.path < other.path) return -1;
        if (this.path > other.path) return 1;
        if (this.authority < other.authority) return -1;
        if (this.authority > other.authority) return 1;
        if (this.scheme < other.scheme) return -1;
        if (this.scheme > other.scheme) return 1;
        if (this.method < other.method) return -1;
        if (this.method > other.method) return 1;
        if (this.pri.urgency !== other.pri.urgency) return this.pri.urgency - other.pri.urgency;
        return this.pri.inc - other.pri.inc;
    }

    toString() {
        return `Request(scheme=${this.scheme}, authority=${this.authority}, path=${this.path}, method=${this.method}, urgency=${this.pri.urgency}, inc=${this.pri.inc})`;
    }
}
