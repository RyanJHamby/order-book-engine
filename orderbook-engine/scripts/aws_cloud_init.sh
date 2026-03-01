#!/bin/bash
set -euo pipefail

REGION="us-east-1"
KEY_NAME="orderbook-key"
SG_NAME="orderbook-sg"
ROLE_NAME="orderbook-benchmark-role"
INSTANCE_PROFILE_NAME="orderbook-benchmark-role"

echo "=== AWS Bootstrap for OrderBook Benchmarks ==="

# 1. Check/create default VPC.
DEFAULT_VPC_ID=$(aws ec2 describe-vpcs \
    --region "$REGION" \
    --query "Vpcs[?IsDefault].VpcId | [0]" \
    --output text)

if [ "$DEFAULT_VPC_ID" == "None" ] || [ -z "$DEFAULT_VPC_ID" ]; then
    echo "Creating default VPC..."
    DEFAULT_VPC_ID=$(aws ec2 create-default-vpc --region "$REGION" --query "Vpc.VpcId" --output text)
    echo "Created VPC: $DEFAULT_VPC_ID"
else
    echo "Default VPC: $DEFAULT_VPC_ID"
fi

# 2. Create security group.
aws ec2 create-security-group \
    --group-name "$SG_NAME" \
    --description "OrderBook benchmark security group" \
    --vpc-id "$DEFAULT_VPC_ID" \
    --region "$REGION" 2>/dev/null || echo "Security group $SG_NAME already exists."

aws ec2 authorize-security-group-ingress \
    --group-name "$SG_NAME" \
    --protocol tcp --port 22 --cidr 0.0.0.0/0 \
    --region "$REGION" 2>/dev/null || echo "SSH rule already exists."

# 3. Create key pair.
if [ ! -f "$KEY_NAME.pem" ]; then
    echo "Creating key pair $KEY_NAME..."
    aws ec2 create-key-pair \
        --key-name "$KEY_NAME" \
        --query 'KeyMaterial' \
        --output text \
        --region "$REGION" > "$KEY_NAME.pem"
    chmod 400 "$KEY_NAME.pem"
else
    echo "Key pair $KEY_NAME.pem already exists."
fi

# 4. Create IAM role for S3 access from EC2.
TRUST_POLICY='{
    "Version": "2012-10-17",
    "Statement": [{
        "Effect": "Allow",
        "Principal": {"Service": "ec2.amazonaws.com"},
        "Action": "sts:AssumeRole"
    }]
}'

aws iam create-role \
    --role-name "$ROLE_NAME" \
    --assume-role-policy-document "$TRUST_POLICY" 2>/dev/null || echo "IAM role already exists."

aws iam attach-role-policy \
    --role-name "$ROLE_NAME" \
    --policy-arn arn:aws:iam::aws:policy/AmazonS3FullAccess 2>/dev/null || true

aws iam create-instance-profile \
    --instance-profile-name "$INSTANCE_PROFILE_NAME" 2>/dev/null || true

aws iam add-role-to-instance-profile \
    --instance-profile-name "$INSTANCE_PROFILE_NAME" \
    --role-name "$ROLE_NAME" 2>/dev/null || true

echo ""
echo "Bootstrap complete:"
echo "  VPC:              $DEFAULT_VPC_ID"
echo "  Security Group:   $SG_NAME"
echo "  Key Pair:         $KEY_NAME.pem"
echo "  IAM Role:         $ROLE_NAME"
echo "  Instance Profile: $INSTANCE_PROFILE_NAME"
echo ""
echo "Run: ./scripts/run_benchmark.sh"
